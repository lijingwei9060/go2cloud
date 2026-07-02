/*
 * main.c — 客户端 (Transfer) 入口
 *
 * go2cloud 块级迁移客户端, 负责:
 *   1. 枚举源磁盘卷
 *   2. (可选) 通过 VSS 创建快照确保数据一致性
 *   3. 逐块读取磁盘数据
 *   4. 计算块哈希, 通过 SQLite 做增量去重
 *   5. MsgPack 编码 + Zstd 压缩 + TCP 帧化发送
 *   6. 接收 ACK 响应, 标记已确认块
 *   7. 处理背压控制、重传、连接管理
 *
 * 模式:
 *   — 子命令模式: client.exe info            (显示磁盘信息)
 *                 client.exe hash <file>     (计算文件哈希)
 *                 client.exe check <disk>    (检查可用性)
 *   — 迁移模式:   client.exe <ip:port>       (开始块级迁移)
 *
 * 配置文件 (可选的第二个命令行参数):
 *   "user.json" 格式:
 *   {
 *     "TailSend":  0,
 *     "HasDump":   1,
 *     "SkipDisks": [3, 4],
 *     "Disks":     {"0": "\\\\.\\PhysicalDrive0", "1": "\\\\.\\PhysicalDrive1"}
 *   }
 *
 * 编译 (Windows — MSVC):
 *   cl /O2 /Fe:client.exe client\*.c /Iinclude ^
 *      /link libzstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
 *
 * 编译 (Windows — MinGW):
 *   gcc -O2 -o client.exe client/*.c -Iinclude -lzstd -lsqlite3 -lole32 -lvssapi -lws2_32
 *
 * 编译 (Linux):
 *   gcc -O2 -o client client/*.c -Iinclude -lzstd -lsqlite3 -lpthread
 */

#include "log.h"
#include "hash.h"
#include "msgpack.h"
#include "wire.h"
#include "queue.h"
#include "pool.h"
#include "timer.h"
#include "sqlite.h"
#include "volume.h"
#include "block_io.h"
#include "vss.h"
#include "../include/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#define SOCKET_ERRNO  WSAGetLastError()
#ifndef SE_PRIVILEGE_ENABLED
#define SE_PRIVILEGE_ENABLED 0x00000002
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET_ERRNO  errno
#endif

/* ================================================================
 * 简易 JSON 解析 (与 server/main.c 逻辑一致)
 * ================================================================ */

#define JSON_MAX_KEY  64

static int json_read_int(const char *json, const char *key, int default_val) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return default_val;
    p = strchr(p + strlen(search), ':');
    if (!p) return default_val;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    return atoi(p);
}

static const char *json_read_str(const char *json, const char *key,
                                 char *out, size_t out_len) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), ':');
    if (!p) return NULL;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    size_t n = MIN((size_t)(end - p), out_len - 1);
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static const char *json_nav(const char *json, const char *parent) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", parent);
    const char *p = json;
    while ((p = strstr(p, search)) != NULL) {
        /* 确保是完整 key 匹配，而非子串 (如 "Disks" 误匹配 "SkipDisks") */
        if (p == json || (*(p - 1) < 'a' || *(p - 1) > 'z')
                      && (*(p - 1) < 'A' || *(p - 1) > 'Z')
                      && *(p - 1) != '_') {
            p = strchr(p + strlen(search), '{');
            return p;
        }
        p++;
    }
    return NULL;
}

static char *read_file_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    if (buf) {
        fread(buf, 1, size, f);
        buf[size] = '\0';
    }
    fclose(f);
    return buf;
}

/* ================================================================
 * 全局状态
 * ================================================================ */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("received signal %d, shutting down...", sig);
        g_running = 0;
    }
}

/* 迁移上下文 */
typedef struct {
    char          server_ip[64];
    uint16_t      server_port;
    int           zstd_level;        /* Zstd 压缩级别 (1~7) */
    int           skip_disks[16];    /* 要跳过的磁盘编号列表 */
    int           skip_count;
    int           tail_send;         /* 是否尾随发送模式 */
    char          db_path[512];      /* SQLite 数据库路径 */
    sqlite_db_t  *db;                /* 块跟踪数据库 (用于 ACK 持久化) */
    socket_pool_t pool;              /* 连接池 */
    send_queue_t  queue;             /* 发送队列 */
    timer_mgr_t   timer;             /* 定时器 */
    msgpack_writer_t mp_writer;     /* MsgPack 编码器 */
    int           all_done;          /* 1 = 增量同步全部完成, 可以退出 */
    int           ctl_end_sent;      /* 1 = ctlEndIncremental 已发送 */
    int           server_done_rcvd;  /* 1 = 已收到 SERVER_DONE */
    int           inc_round;         /* 增量轮次计数 */
    int           inc_version;       /* 当前 T_VERSION 版本号 */
    int           zero_change_rounds;/* 连续无变化轮次数 */
    int           final_verify;      /* 1 = 正在执行最终验证轮 */
    int           dry_run;           /* 1 = dry-run mode, no network I/O */
    int           rate_limit_mb;      /* 速率限制 (MB/s), 0 = 不限速 */
    uint64_t      rate_bytes_sent;    /* 当前速率窗口内已发送字节数 */
    uint64_t      rate_window_start;  /* 当前速率窗口起始时间 (ms) */
    uint64_t      total_blocks;       /* 所有分区待发送块总数 */
} migrate_ctx_t;

/* ================================================================
 * 子命令: info — 显示磁盘信息
 * ================================================================ */

static int cmd_info(void) {
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0) {
        printf("No fixed disks found.\n");
        printf("Possible causes:\n");
        printf("  - Not running as Administrator (required for disk access)\n");
        printf("  - No fixed disks in this system\n");
        return 1;
    }

    printf("Volume Information:\n");
    printf("%-6s %-8s %-16s %-16s %s\n", "Disk", "Size(GB)", "TotalBlocks", "Name", "DiskPath");
    printf("------ ------ ---------------- ---------------- ----\n");

    for (int i = 0; i < vol_list.count; i++) {
        volume_info_t *v = &vol_list.volumes[i];
        printf("%-6d %-8.2f %-16llu %-16s %s\n",
               v->devno,
               (double)v->total_bytes / (1024.0 * 1024.0 * 1024.0),
               (unsigned long long)v->block_count,
               v->name, v->disk_path);
    }
    return 0;
}

/* ================================================================
 * 子命令: end_session — 清理块跟踪数据
 * ================================================================ */

static int cmd_end_session(const char *db_path) {
    sqlite_db_t *db = sqlite_open(db_path);
    if (!db) {
        printf("cannot open database: %s\n", db_path);
        return 1;
    }
    int rc = sqlite_clear_all_blocks(db);
    sqlite_close(db);
    return rc == 0 ? 0 : 1;
}

/* ================================================================
 * 子命令: sentbytes — 查询已确认总字节数 (跨进程进度)
 * ================================================================ */

static int cmd_sentbytes(const char *db_path) {
    sqlite_db_t *db = sqlite_open(db_path);
    if (!db) {
        printf("0\n");
        return 1;
    }
    int64_t total = sqlite_total_acked_bytes(db);
    if (total < 0) total = 0;
    printf("%lld\n", (long long)total);
    sqlite_close(db);
    return 0;
}

/* ================================================================
 * 子命令: blockinfo — 查询块跟踪数据库
 *
 *   client.exe blockinfo [flags] <db_path>                    整体摘要
 *   client.exe blockinfo [flags] <db_path> <devno>            按磁盘列出
 *   client.exe blockinfo <db_path> <devno> <offset>           单个块详情
 *   flags: --pending (ack=0), --acked (ack=1)
 * ================================================================ */

static void print_block_detail(const block_info_t *b) {
    char hash_str[32];
    snprintf(hash_str, sizeof(hash_str), "0x%016llx",
             (unsigned long long)b->hash);

    printf("  devno      = %d\n",   (int)b->devno);
    printf("  offset     = %lld",   (long long)b->offset);
    /* 当偏移量为 BLOCK_SIZE 的整数倍时显示块索引 */
    if (b->offset % BLOCK_SIZE == 0) {
        printf("  (block #%lld)", (long long)(b->offset / BLOCK_SIZE));
    }
    printf("\n");
    printf("  size       = %d",     (int)b->size);
    if (b->size > 0) {
        double mb = (double)b->size / (1024.0 * 1024.0);
        printf("  (%.2f MB)", mb);
    }
    printf("\n");
    printf("  hash       = %s\n",   hash_str);
    printf("  version    = %d",     b->version);
    if (b->version == 0) {
        printf("  (full sync)\n");
    } else {
        printf("  (incremental round %d)\n", b->version);
    }
    printf("  ack        = %d",     b->ack);
    printf("  (%s)\n",              b->ack ? "confirmed" : "pending");
    printf("  last_sent  = %lld\n", (long long)b->last_sent);
    if (b->remote_id[0]) {
        printf("  remote_id  = %s\n", b->remote_id);
    }
}

static int cmd_blockinfo(int argc, char *argv[]) {
    const char *db_path = "tracker.db";
    int filter_devno = -1;
    int64_t filter_offset = -1;
    int has_offset = 0;
    int filter_ack   = -1;  /* -1=all, 0=pending, 1=acked */
    int filter_version = -1; /* -1=all, >=0 filter by version */
    int show_history = 0;

    /* 解析参数 */
    int arg_idx = 2;
    while (argc > arg_idx && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "--pending") == 0) {
            filter_ack = 0;
        } else if (strcmp(argv[arg_idx], "--acked") == 0) {
            filter_ack = 1;
        } else if (strcmp(argv[arg_idx], "--all") == 0) {
            filter_ack = -1;
        } else if (strcmp(argv[arg_idx], "--history") == 0) {
            show_history = 1;
        } else if (strncmp(argv[arg_idx], "--version=", 10) == 0) {
            filter_version = atoi(argv[arg_idx] + 10);
        }
        arg_idx++;
    }
    if (argc > arg_idx && strchr(argv[arg_idx], '.') != NULL) {
        db_path = argv[arg_idx];
        arg_idx++;
    }
    if (argc > arg_idx) {
        filter_devno = atoi(argv[arg_idx++]);
    }
    if (argc > arg_idx) {
        filter_offset = (int64_t)atoll(argv[arg_idx++]);
        has_offset = 1;
    }

    sqlite_db_t *db = sqlite_open(db_path);
    if (!db) {
        printf("ERROR: cannot open database: %s\n", db_path);
        return 1;
    }

    if (has_offset) {
        /* ——— 单个块详情 ——— */
        block_info_t info;
        int rc = sqlite_get_block_info(db, (int32_t)filter_devno,
                                       filter_offset, &info);
        if (rc == 0) {
            printf("Block Info (%s):\n", db_path);
            print_block_detail(&info);
        } else if (rc == -1) {
            printf("Block not found: devno=%d offset=%lld\n",
                   filter_devno, (long long)filter_offset);
        } else {
            printf("Database error\n");
        }
    } else if (filter_devno >= 0) {
        /* ——— 按磁盘列出 ——— */
        int total = sqlite_count_blocks(db, filter_devno, -1);
        int acked = sqlite_count_blocks(db, filter_devno, 1);
        int pending = sqlite_count_blocks(db, filter_devno, 0);
        const char *label = filter_ack == 0 ? "pending" :
                           filter_ack == 1 ? "confirmed" : "total";
        printf("Disk %d: %d blocks %s (%d confirmed, %d pending)\n",
               filter_devno,
               filter_ack == 0 ? pending : filter_ack == 1 ? acked : total,
               label, acked, pending);

        if (total > 0) {
            printf("%-8s %-16s %-10s %-18s %-3s %-3s\n",
                   "devno", "offset", "size", "hash", "ack", "ver");
            printf("-------- ---------------- ---------- ------------------ --- ---\n");

            int cap = filter_ack == 0 ? pending : filter_ack == 1 ? acked : total;
            if (cap > 0) {
            block_info_t *list = calloc((size_t)total, sizeof(block_info_t));
            if (list) {
                int n = sqlite_list_blocks(db, filter_devno, list, total);
                for (int i = 0; i < n; i++) {
                    if (filter_ack >= 0 && list[i].ack != filter_ack) continue;
                    if (filter_version >= 0 && list[i].version != filter_version) continue;
                    char hash_str[32];
                    snprintf(hash_str, sizeof(hash_str), "0x%016llx",
                             (unsigned long long)list[i].hash);
                    printf("%-8d %-16lld %-10d %-18s %-3d %-3d\n",
                           (int)list[i].devno,
                           (long long)list[i].offset,
                           (int)list[i].size,
                           hash_str,
                           list[i].ack,
                           list[i].version);
                }
                free(list);
            }
            }
        }
    } else {
        /* ——— 整体摘要 ——— */
        int total = sqlite_count_blocks(db, -1, -1);
        int acked = sqlite_count_blocks(db, -1, 1);
        int pending = sqlite_count_blocks(db, -1, 0);
        int64_t acked_bytes = sqlite_total_acked_bytes(db);

        printf("Database: %s\n", db_path);
        printf("  Total blocks:  %d\n", total);
        printf("  Confirmed:     %d\n", acked);
        printf("  Pending:       %d\n", pending);
        printf("  Acked bytes:   %lld", (long long)acked_bytes);
        if (acked_bytes > 0) {
            double gb = (double)acked_bytes / (1024.0 * 1024.0 * 1024.0);
            printf("  (%.2f GB)", gb);
        }
        printf("\n");

        /* 版本历史 */
        if (show_history) {
            version_info_t vinfos[64];
            int vn = sqlite_get_version_history(db, NULL, vinfos, 64);
            if (vn > 0) {
                printf("\n  Version History:\n");
                printf("  %-5s %-20s %-20s %-8s %-8s %-10s\n",
                       "Ver", "Start Time", "End Time", "Scanned", "Changed", "Duration");
                printf("  ----- -------------------- -------------------- -------- -------- ----------\n");
                for (int i = 0; i < vn; i++) {
                    char start_buf[32], end_buf[32];
                    time_t st = (time_t)(vinfos[i].start_time / 1000);
                    time_t et = (time_t)(vinfos[i].end_time / 1000);
                    struct tm tm_st, tm_et;
#ifdef _WIN32
                    localtime_s(&tm_st, &st);
                    localtime_s(&tm_et, &et);
#else
                    localtime_r(&st, &tm_st);
                    localtime_r(&et, &tm_et);
#endif
                    strftime(start_buf, sizeof(start_buf), "%Y-%m-%d %H:%M:%S", &tm_st);
                    strftime(end_buf, sizeof(end_buf), "%Y-%m-%d %H:%M:%S", &tm_et);
                    int64_t duration_s = vinfos[i].end_time > vinfos[i].start_time
                        ? (vinfos[i].end_time - vinfos[i].start_time) / 1000 : 0;
                    printf("  %-5d %-20s %-20s %-8d %-8d %-10llds\n",
                           vinfos[i].version,
                           start_buf, end_buf,
                           vinfos[i].scanned_count,
                           vinfos[i].changed_count,
                           (long long)duration_s);
                }
            } else {
                printf("\n  No version history found.\n");
            }
        }

        /* 按磁盘分别统计 */
        if (total > 0) {
            printf("\n  Per-disk breakdown:\n");
            printf("  %-6s %-8s %-8s %-8s\n",
                   "disk", "total", "acked", "pending");
            printf("  ------ -------- -------- --------\n");
            for (int d = 0; d < 16; d++) {
                int dt = sqlite_count_blocks(db, d, -1);
                if (dt == 0) continue;
                int da = sqlite_count_blocks(db, d, 1);
                int dp = sqlite_count_blocks(db, d, 0);
                printf("  %-6d %-8d %-8d %-8d\n", d, dt, da, dp);
            }
        }

        /* 按版本统计块数 */
        if (total > 0 && !show_history) {
            version_info_t vinfos[64];
            int vn = sqlite_get_version_history(db, NULL, vinfos, 64);
            if (vn > 0) {
                printf("\n  Per-version block count:\n");
                printf("  %-5s %-8s\n", "Ver", "Blocks");
                printf("  ----- --------\n");
                for (int i = 0; i < vn; i++) {
                    int bc = sqlite_count_blocks_by_version(db, vinfos[i].version, NULL);
                    printf("  %-5d %-8d\n", vinfos[i].version, bc);
                }
            }
        }
    }

    sqlite_close(db);
    return 0;
}

/* ================================================================
 * 子命令: test_vss — 验证 VSS 快照功能
 * ================================================================ */

static int cmd_test_vss(void) {
    vss_context_t *ctx = vss_init_ex(0x1d /* VSS_CTX_CLIENT_ACCESSIBLE */);
    if (!ctx) {
        printf("VSS init failed\n");
        return 1;
    }

    const char *volumes[] = {"C:\\", NULL};
    vss_snapshot_t snaps[VSS_MAX_VOLUMES];
    memset(snaps, 0, sizeof(snaps));

    int n = vss_create_snapshots(ctx, volumes, snaps);
    if (n <= 0) {
        printf("Snapshot creation failed\n");
        vss_cleanup(ctx);
        return 1;
    }

    printf("Using VSS volume: %s\n", snaps[0].snapshot_path);

    vss_backup_complete(ctx);
    vss_cleanup(ctx);
    return 0;
}

static int cmd_vss_query(void) {
    vss_snapshot_info_t info[64];
    memset(info, 0, sizeof(info));

    int n = vss_query_snapshots(info, 64);
    if (n < 0) {
        printf("vss_query failed\n");
        return 1;
    }

    if (n == 0) {
        printf("No snapshots found.\n");
        return 0;
    }

    for (int i = 0; i < n; i++) {
        printf("Shadow Copy ID: %s\n", info[i].snapshot_id_str);
        printf("  Original Volume: %s\n", info[i].original_volume);
        printf("  Device Object:   %s\n", info[i].snapshot_path);
        printf("  Creation Time:   %s\n", info[i].creation_time);
        printf("\n");
    }

    return 0;
}

static int cmd_vss_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: vss_delete <guid> | --all\n");
        return 1;
    }

    if (strcmp(argv[2], "--all") == 0) {
        int deleted = vss_delete_all_snapshots();
        if (deleted < 0) {
            printf("vss_delete --all failed\n");
            return 1;
        }
        printf("Deleted %d snapshot(s).\n", deleted);
        return 0;
    }

    int deleted = vss_delete_snapshot(argv[2]);
    if (deleted < 0) {
        printf("vss_delete failed\n");
        return 1;
    }
    printf("Deleted %d snapshot(s).\n", deleted);
    return 0;
}

/* ================================================================
 * 子命令: isbios — 检测固件类型 (UEFI / Legacy)
 * ================================================================ */

static int cmd_isbios(void) {
#ifdef _WIN32
    /* 启用固件环境变量访问权限 */
    HANDLE token;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp;
        LUID luid;
        if (LookupPrivilegeValueA(NULL, "SeSystemEnvironmentPrivilege", &luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
            // 严格来说这里可以用 GetLastError() == ERROR_SUCCESS 校验，但即使失败也可以继续往下走
        }
        CloseHandle(token);
    }

    /* 2. 优先使用 GetFirmwareType (Windows 8 / Server 2012 以上) */
    /*
     * GetFirmwareType (Windows 8+):
     *   FirmwareTypeBios = 1 → Legacy BIOS
     *   FirmwareTypeUefi = 2 → UEFI
     */
    int is_uefi = 0;
    FIRMWARE_TYPE ft = FirmwareTypeUnknown;

    /* 动态获取 GetFirmwareType (Windows 8+ kernel32.dll) */
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (k32) {
        BOOL (WINAPI *pfnGetFirmwareType)(FIRMWARE_TYPE *) =
            (BOOL (WINAPI *)(FIRMWARE_TYPE *))GetProcAddress(k32, "GetFirmwareType");
        if (pfnGetFirmwareType && pfnGetFirmwareType(&ft)) {
            if (ft == FirmwareTypeUefi) {
                is_uefi = 1;
            } else if (ft == FirmwareTypeBios) {
                is_uefi = 0;
            }
        }
    }

   /* 3. 回退方案: 如果 GetFirmwareType 无法确定 (如 Win7), 探测 UEFI 变量 */
    if (ft == FirmwareTypeUnknown) {
        DWORD result = GetFirmwareEnvironmentVariableA("",
            "{00000000-0000-0000-0000-000000000000}", NULL, 0);
        if (result == 0) {
            // 调用失败才检查错误码: ERROR_INVALID_FUNCTION = 固件不支持 = Legacy BIOS
            if (GetLastError() != ERROR_INVALID_FUNCTION) {
                is_uefi = 1;
            }
        } else {
            is_uefi = 1;
        }
    }

    printf("%s\n", is_uefi ? "UEFI" : "Legacy");
#else
    // 跨平台处理（例如 Linux）
    #if defined(__linux__)
    if (access("/sys/firmware/efi", F_OK) == 0) {
        printf("UEFI\n");
        return 1;
    }
    #endif
    printf("Legacy\n");
#endif
    return 0;
}

/* ================================================================
 * 子命令: begin_session — 标记会话开始
 * ================================================================ */

static int cmd_begin_session(void) {
    return 0;
}

/* ================================================================
 * 子命令: hash <file> — 计算文件哈希
 * ================================================================ */

static int cmd_hash(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        printf("Cannot open: %s\n", filepath);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        printf("Empty or invalid file: %s\n", filepath);
        fclose(f);
        return 1;
    }

    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        printf("Out of memory\n");
        fclose(f);
        return 1;
    }

    fread(buf, 1, (size_t)size, f);
    fclose(f);

    uint64_t h = hash_block(buf, (size_t)size, 0);

    printf("hash(%s) = 0x%016llx  (size=%ld, seed=0)\n",
           filepath, (unsigned long long)h, size);

    free(buf);
    return 0;
}

/* ================================================================
 * 子命令: check <disk> — 检查磁盘可访问性
 * ================================================================ */

static int cmd_check(const char *disk_path) {
    block_reader_t *r = block_reader_open(disk_path);
    if (!r) {
        printf("Cannot open disk: %s\n", disk_path);
        return 1;
    }

    uint64_t size = block_reader_size(r);
    uint64_t blocks = block_reader_block_count(r, (uint32_t)BLOCK_SIZE);

    printf("Disk: %s\n", disk_path);
    printf("  Total size:   %.2f GB\n", (double)size / (1024.0 * 1024.0 * 1024.0));
    printf("  Total blocks: %llu (@ 1MB)\n", (unsigned long long)blocks);

    /* 测试读取第一块 */
    uint8_t *buf = malloc((size_t)BLOCK_SIZE);
    if (buf) {
        uint32_t n_read = 0;
        if (block_reader_read(r, 0, buf, (uint32_t)BLOCK_SIZE, &n_read) == 0) {
            uint64_t h = hash_block(buf, n_read, 0);
            printf("  First block:  %u bytes, hash=0x%016llx\n", n_read,
                   (unsigned long long)h);
            printf("  Status:       OK\n");
        } else {
            printf("  Status:       READ ERROR\n");
        }
        free(buf);
    }

    block_reader_close(r);
    return 0;
}

/* ================================================================
 * 迁移核心流程
 * ================================================================ */

/*
 * 接收并处理一条 ACK 响应。
 * 非阻塞: 无数据时立即返回。
 */
static void process_ack(migrate_ctx_t *ctx) {
    if (ctx->dry_run) {
        int32_t  devnos[256];
        int64_t  offsets[256];
        int n = queue_get_pending(&ctx->queue, devnos, offsets, 256);
        for (int i = 0; i < n; i++) {
            queue_ack(&ctx->queue, devnos[i], offsets[i]);
            if (ctx->db && !ctx->tail_send) {
                sqlite_block_mark_acked(ctx->db, devnos[i], offsets[i]);
            }
        }
        return;
    }

    server_response_t resp;

    for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
        pool_conn_t *c = &ctx->pool.conns[i];
        if (c->fd == SOCKET_ERR) continue;

        while (1) {
            int rc = pool_recv_ack(c, &resp);
            if (rc <= 0) break;  /* 无数据或错误 */

            if (resp.type == RESPONSE_ACK || resp.type == RESPONSE_ACK_ALT) {
                LOG_DEBUG("ACK: devno=%d offset=%lld size=%d",
                          resp.devno, (long long)resp.offset, resp.size);
                queue_ack(&ctx->queue, resp.devno, resp.offset);
                /* 全量模式: 服务端 ACK 即确认, 直接标记 ack=1。
                 * 增量模式 (TailSend): sck=0 保持不变, 由增量轮的
                 * live disk hash 对比来确认稳定性后才标记 ack=1。 */
                if (ctx->db && !ctx->tail_send) {
                    sqlite_block_mark_acked(ctx->db, resp.devno, resp.offset);
                }
            } else if (resp.type == RESPONSE_SERVER_DONE) {
                LOG_INFO("SERVER_DONE received from server");
                ctx->server_done_rcvd = 1;
            } else if (resp.type == RESPONSE_BINLOG) {
                LOG_WARN("BINLOG alert: devno=%d size=%d",
                         resp.devno, resp.size);
            }
        }
    }
}

/*
 * 发送一个块。
 * 流程: 读取 → 哈希 → 去重检查 → 编码 → 入队 → 发送
 *
 * disk_offset:   物理磁盘上的绝对字节偏移 (用于协议/SQLite)
 * reader_offset: 读取器上的字节偏移 (VSS 读取器为卷内偏移,
 *                 PhysicalDrive 读取器为绝对偏移)
 */
static int send_block(migrate_ctx_t *ctx, block_reader_t *reader,
                      sqlite_db_t *db, int32_t devno,
                      uint64_t disk_offset, uint64_t reader_offset,
                      uint8_t *data_buf) {

    uint32_t data_len = 0;
    if (block_reader_read(reader, reader_offset, data_buf,
                          (uint32_t)BLOCK_SIZE, &data_len) != 0) {
        LOG_ERROR("read block failed: devno=%d disk_offset=%llu reader_offset=%llu",
                  devno, (unsigned long long)disk_offset,
                  (unsigned long long)reader_offset);
        return -1;
    }

    if (data_len == 0) {
        return 0; /* EOF */
    }

    /* 崩溃恢复: 如果此块已 ACK (上次会话遗留), 跳过 */
    if (sqlite_block_acked(db, devno, (int64_t)disk_offset) == 1) {
        LOG_DEBUG("skip acked: devno=%d offset=%llu (recovered from prior session)",
                  devno, (unsigned long long)disk_offset);
        return 0;
    }

    /* 计算哈希 (使用 disk_offset 的低 32 位作为种子的组成部分) */
    uint64_t h = hash_block(data_buf, data_len, disk_offset & 0xFFFFFFFF);
    LOG_INFO("send_block: devno=%d offset=%llu size=%u hash=0x%016llx",
             devno, (unsigned long long)disk_offset, data_len,
             (unsigned long long)h);

    /* 增量去重: 仅 TailSend 模式下查询 SQLite 对比 hash */
    if (ctx->tail_send) {
        uint64_t stored_hash = 0;
        if (sqlite_block_lookup(db, devno, (int64_t)disk_offset, &stored_hash) == 0) {
            if (stored_hash == h) {
                LOG_DEBUG("skip: devno=%d offset=%llu (hash unchanged 0x%016llx)",
                          devno, (unsigned long long)disk_offset,
                          (unsigned long long)h);
                return 0; /* 跳过未变化的块 */
            }
        }
    }

    /* MsgPack 编码 */
    if (msgpack_encode_block(&ctx->mp_writer, devno, (int64_t)disk_offset,
                              data_buf, data_len) != 0) {
        LOG_ERROR("msgpack encode failed: devno=%d offset=%llu",
                  devno, (unsigned long long)disk_offset);
        return -1;
    }

    /* 入队 (带背压重试: 队列满时排空并等待后重试, 不丢弃块) */
    int rc;
    int retry = 0;
    while ((rc = queue_push(&ctx->queue, devno, (int64_t)disk_offset, h,
                            ctx->mp_writer.buf, ctx->mp_writer.written)) != 0) {
        if (!g_running) return -1;

        if (rc == 2 || rc == -1) {
            /* 排空队列 + 处理 ACK 释放已发送块 */
            while (flush_one(ctx) > 0) { }
            process_ack(ctx);

#ifdef _WIN32
            Sleep(BACKPRESSURE_SLEEP_MS);
#else
            usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
            retry++;
            if (retry > 600) {
                /* 30s timeout — force drain and retry one last time */
                LOG_ERROR("queue stuck for 30s, forcing drain");
                while (queue_count(&ctx->queue) > 0 && g_running) {
                    while (flush_one(ctx) > 0) { }
                    process_ack(ctx);
#ifdef _WIN32
                    Sleep(BACKPRESSURE_SLEEP_MS);
#else
                    usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
                }
                retry = 0;
            }
        }
    }

    /* 更新 SQLite */
    sqlite_block_upsert_v(db, devno, (int64_t)disk_offset,
                          (int32_t)data_len, h, 0, ctx->inc_version);

    return 0;
}

/*
 * 从队列中取出一条消息并通过一个连接发送。
 */
static int flush_one(migrate_ctx_t *ctx) {
    queue_entry_t entry;
    if (queue_pop(&ctx->queue, &entry) != 0) {
        return 0; /* 队列空 */
    }

    if (ctx->dry_run) {
        queue_ack(&ctx->queue, entry.devno, entry.offset);
        if (ctx->db && !ctx->tail_send) {
            sqlite_block_mark_acked(ctx->db, entry.devno, entry.offset);
        }
        return 1;
    }

    /* 获取一个连接 */
    pool_conn_t *conn = pool_acquire(&ctx->pool);
    if (!conn) {
        /* 无可用连接, 重新入队暂不支持 — 简单丢弃 */
        LOG_WARN("no available connection, block dropped");
        return -1;
    }

    /* 通过有线协议发送 */
    int rc = wire_send(conn->fd,
                       entry.payload, entry.payload_len,
                       ctx->zstd_level,
                       conn->send_scratch, conn->send_scratch_cap);

    pool_touch(conn);

    if (rc != 0) {
        LOG_ERROR("wire_send failed on fd=%d", (int)conn->fd);
        pool_release(&ctx->pool, conn);
        return -1;
    }

    conn->blocks_sent++;
    conn->bytes_sent += entry.payload_len;

    pool_release(&ctx->pool, conn);

    /* 速率控制: 超过 RateLimitMB 则短暂休眠 */
    if (ctx->rate_limit_mb > 0) {
        ctx->rate_bytes_sent += (uint64_t)entry.payload_len;
        uint64_t now = timer_now_ms();
        if (ctx->rate_window_start == 0) ctx->rate_window_start = now;

        double bytes_per_ms = (double)ctx->rate_limit_mb * 1048576.0 / 1000.0;
        if (bytes_per_ms > 0.0) {
            uint64_t expected_ms = (uint64_t)((double)ctx->rate_bytes_sent
                                              / bytes_per_ms);
            uint64_t elapsed_ms = now - ctx->rate_window_start;
            if (elapsed_ms < expected_ms) {
                uint64_t sleep_ms = expected_ms - elapsed_ms;
                if (sleep_ms > 200) sleep_ms = 200;
#ifdef _WIN32
                Sleep((DWORD)sleep_ms);
#else
                usleep((useconds_t)(sleep_ms * 1000));
#endif
            }
        }
        if (now - ctx->rate_window_start > 5000) {
            ctx->rate_window_start = now;
            ctx->rate_bytes_sent = 0;
        }
    }

    return 1;
}

/* 读取器条目: 捆绑读取器及其分区元数据 */
typedef struct {
    block_reader_t *reader;           /* 全量同步读取器 (VSS 或 PhysicalDrive) */
    block_reader_t *live_reader;      /* 增量同步读取器 (始终 PhysicalDrive) */
    int32_t         devno;             /* 所在物理磁盘编号 */
    uint64_t        partition_offset;  /* 分区在磁盘上的绝对偏移 */
    uint64_t        block_count;       /* 分区实际块数 (非全盘) */
    int             is_vss;            /* 1 = 读取器打开 VSS 快照路径, 0 = PhysicalDrive */
    char            name[16];          /* 分区名 (如 "C:") */
    int             part_index;        /* 分区在磁盘上的索引 (1-based) */
    uint64_t        sent_count;        /* 本分区已发送块数 */
} reader_entry_t;

/*
 * 处理重传: 扫描 SQLite 中未 ACK 的块, 对超时的块进行重传。
 */
static void do_retransmit(migrate_ctx_t *ctx, sqlite_db_t *db,
                          reader_entry_t *readers, int reader_count,
                          uint8_t *data_buf) {
    int32_t  devnos[256];
    int64_t  offsets[256];
    int count = sqlite_get_unacked(db, devnos, offsets, 256, "");

    LOG_DEBUG("retransmit scan: %d unacked blocks", count);

    for (int i = 0; i < count; i++) {
        if (!g_running) break;

        uint64_t disk_offset = (uint64_t)offsets[i];
        int32_t devno = devnos[i];

        /* 查找对应此 devno 且覆盖此 disk_offset 的读取器。
         * VSS 卷内偏移 = disk_offset - partition_offset (卷起始偏移 0 对应分区起始)。
         * PhysicalDrive 偏移 = disk_offset (绝对磁盘偏移)。 */
        reader_entry_t *entry = NULL;
        uint64_t reader_offset = disk_offset;
        for (int r = 0; r < reader_count; r++) {
            if (readers[r].devno != devno) continue;
            uint64_t part_end = readers[r].partition_offset
                              + block_reader_size(readers[r].reader);
            if (disk_offset >= readers[r].partition_offset
                && disk_offset < part_end) {
                entry = &readers[r];
                reader_offset = entry->is_vss
                    ? (disk_offset - entry->partition_offset)
                    : disk_offset;
                break;
            }
        }
        if (!entry) {
            LOG_WARN("retransmit: no reader for devno=%d offset=%llu",
                     devno, (unsigned long long)disk_offset);
            continue;
        }

        LOG_INFO("retransmit: devno=%d offset=%lld",
                 devno, (long long)disk_offset);
        send_block(ctx, entry->reader, db, devno,
                   disk_offset, reader_offset, data_buf);
    }
}

/*
 * 检查并重建断开的连接。
 */
static void do_reconnect(migrate_ctx_t *ctx) {
    int stale = pool_reap_stale(&ctx->pool);
    if (stale > 0) {
        LOG_INFO("reconnect: closed %d stale connections", stale);
    }

    /* 如果连接数低于目标, 尝试获取新连接 (pool_acquire 会自动创建) */
    while (pool_count(&ctx->pool) < SOCKET_POOL_TARGET) {
        pool_conn_t *c = pool_acquire(&ctx->pool);
        if (c) {
            pool_release(&ctx->pool, c);
        } else {
            break;
        }
    }
}

/*
 * Fisher-Yates 洗牌: 将前 n 个元素原地随机打乱。
 * 用于增量同步中随机化未确认块的处理顺序, 避免顺序 I/O。
 */
static void shuffle_blocks(int32_t *devnos, int64_t *offsets,
                           uint64_t *hashes, int64_t *last_sents, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        /* swap devnos */
        int32_t tmp_d = devnos[i]; devnos[i] = devnos[j]; devnos[j] = tmp_d;
        /* swap offsets */
        int64_t tmp_o = offsets[i]; offsets[i] = offsets[j]; offsets[j] = tmp_o;
        /* swap hashes */
        uint64_t tmp_h = hashes[i]; hashes[i] = hashes[j]; hashes[j] = tmp_h;
        /* swap last_sents */
        int64_t tmp_l = last_sents[i]; last_sents[i] = last_sents[j]; last_sents[j] = tmp_l;
    }
}

/*
 * 增量同步 — 单轮定时器回调 (18s 间隔)。
 *
 * 从 live disk (PhysicalDrive) 重读所有未 ACK 块,
 * 重新计算 hash 与 T_BLOCK 中存储的旧 hash 比较:
 *   - hash 相同 → 块未变化, 跳过
 *   - hash 不同 → 块已变化, 重新压缩发送
 *
 * 每块有 121s 冷却期: 同一块在此时间内不会重复重读。
 *
 * live_readers: 指向 PhysicalDrive 读取器数组 (用于读 live disk)
 * vss_readers:  指向原始读取器数组 (用于定位分区)
 * reader_count: 读取器数量
 * data_buf:     块读取缓冲区 (≥ 1MB)
 * db:           SQLite 块跟踪数据库
 */
static int do_incremental_round(migrate_ctx_t *ctx,
                                  reader_entry_t *live_readers,
                                  reader_entry_t *vss_readers,
                                  int reader_count,
                                  sqlite_db_t *db, uint8_t *data_buf,
                                  int current_version) {
    int32_t  devnos[4096];
    int64_t  offsets[4096];
    uint64_t hashes[4096];
    int64_t  last_sents[4096];
    char remote_id[256];
    snprintf(remote_id, sizeof(remote_id), "%s:%d",
             ctx->server_ip, (int)ctx->server_port);

    int count = sqlite_get_unacked_with_hash(db, devnos, offsets,
                                              hashes, last_sents,
                                              4096, remote_id);
    if (count <= 0) {
        LOG_DEBUG("incremental round: no unacked blocks");
        return 0;
    }

    /* Fisher-Yates 随机打乱, 避免顺序 I/O 导致磁盘缓存命中偏差 */
    shuffle_blocks(devnos, offsets, hashes, last_sents, count);

    int64_t now_ms = (int64_t)timer_now_ms();
    int requeued = 0;

    for (int i = 0; i < count && g_running; i++) {
        int32_t  devno      = devnos[i];
        uint64_t disk_offset = (uint64_t)offsets[i];
        uint64_t old_hash   = hashes[i];
        int64_t  last_sent  = last_sents[i];

        /* 121 秒冷却期: 同一块不频繁重读 */
        if (last_sent > 0 && (now_ms - last_sent) < RETRANSMIT_MIN_INTERVAL_SEC * 1000) {
            continue;
        }

        /* 背压: 队列深度过大或内存不足时暂停, 剩余块留待下一轮 */
        if (queue_count(&ctx->queue) > 9) {
            LOG_DEBUG("incremental: backpressure (queue=%d), deferring %d blocks",
                     queue_count(&ctx->queue), count - i);
            break;
        }

        /* 查找此 block 对应的 live disk 读取器 */
        reader_entry_t *entry = NULL;
        uint64_t reader_offset = disk_offset;
        for (int r = 0; r < reader_count; r++) {
            /* 使用 vss_readers 的 partition_offset 做分区匹配 */
            if (vss_readers[r].devno != devno) continue;
            uint64_t part_end = vss_readers[r].partition_offset
                              + block_reader_size(vss_readers[r].reader);
            if (disk_offset >= vss_readers[r].partition_offset
                && disk_offset < part_end) {
                entry = &live_readers[r];
                /* live disk 读取: 使用绝对磁盘偏移 (不是 VSS 卷内偏移) */
                reader_offset = disk_offset;
                break;
            }
        }
        if (!entry) continue;

        /* 使用 live disk reader (始终 PhysicalDrive) 读取块数据 */
        block_reader_t *live_r = entry->live_reader ? entry->live_reader : entry->reader;
        if (!live_r) continue;

        uint32_t data_len = 0;
        if (block_reader_read(live_r, reader_offset, data_buf,
                              (uint32_t)BLOCK_SIZE, &data_len) != 0) {
            LOG_WARN("incremental: read live disk failed devno=%d offset=%llu",
                     devno, (unsigned long long)disk_offset);
            continue;
        }
        if (data_len == 0) continue;

        /* 计算新 hash 并对比 */
        uint64_t new_hash = hash_block(data_buf, data_len, disk_offset & 0xFFFFFFFF);
        if (new_hash == old_hash) {
            /* 块未变化 — live disk 与已发送数据一致, 标记为稳定 (ack=1) */
            if (current_version > 0) {
                sqlite_block_mark_verified(db, devno, (int64_t)disk_offset,
                                           current_version);
            } else {
                sqlite_block_mark_acked(db, devno, (int64_t)disk_offset);
            }
            /* 更新 last_sent: 确保 ORDER BY last_sent 将此块排到队尾,
             * 让本轮尚未检查的块 (如高 offset 的 D 盘) 下轮优先被选中 */
            sqlite_update_last_sent(db, devno, (int64_t)disk_offset, now_ms);
            continue;
        }

        LOG_INFO("incremental: changed block devno=%d offset=%llu "
                  "old_hash=0x%016llx new_hash=0x%016llx",
                  devno, (unsigned long long)disk_offset,
                  (unsigned long long)old_hash, (unsigned long long)new_hash);

        /* 更新 last_sent 时间戳 (冷却期) */
        sqlite_update_last_sent(db, devno, (int64_t)disk_offset, now_ms);

        /* MsgPack 编码 → 入队 (带背压重试) */
        if (msgpack_encode_block(&ctx->mp_writer, devno, (int64_t)disk_offset,
                                  data_buf, data_len) != 0) {
            continue;
        }

        int rc;
        int retry_count = 0;
        while ((rc = queue_push(&ctx->queue, devno, (int64_t)disk_offset,
                                 new_hash, ctx->mp_writer.buf,
                                 ctx->mp_writer.written)) != 0) {
            if (!g_running) return requeued;
            while (flush_one(ctx) > 0) { }
            process_ack(ctx);
#ifdef _WIN32
            Sleep(BACKPRESSURE_SLEEP_MS);
#else
            usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
            if (++retry_count > 300) break;  /* 15s timeout */
        }
        if (rc == 0) {
            /* 更新 SQLite: 新 hash + ack=0 (等待确认) */
            sqlite_block_upsert_v(db, devno, (int64_t)disk_offset,
                                  (int32_t)data_len, new_hash, 0,
                                  current_version);
            sqlite_update_last_sent(db, devno, (int64_t)disk_offset,
                                     (int64_t)timer_now_ms());
            requeued++;
        }
    }

    if (requeued > 0) {
        LOG_INFO("incremental round: %d blocks re-sent (%d total unacked)",
                 requeued, count);
    }
    return requeued;
}

/*
 * timer_cb_real: 发送 ctlEndIncremental, 等待 SERVER_DONE, 设置 allDone=1。
 *
 * 仅在明确需要结束时调用 (已通过 should_finish 检查)。
 * 返回 1 表示 allDone 已设置, 0 表示失败需重试。
 */
static int timer_cb_real(migrate_ctx_t *ctx) {
    if (ctx->dry_run) {
        /* 排空队列 */
        while (queue_count(&ctx->queue) > 0 && g_running) {
            while (flush_one(ctx) > 0) { }
            process_ack(ctx);
        }
        ctx->all_done = 1;
        LOG_INFO("dry-run: allDone set, migration complete");
        return 1;
    }

    /* 排空队列: 确保所有块都已发送 */
    while (queue_count(&ctx->queue) > 0 && g_running) {
        while (flush_one(ctx) > 0) { }
        process_ack(ctx);
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
    }

    /* 发送 ctlEndIncremental */
    if (!ctx->ctl_end_sent) {
        pool_conn_t *c = pool_acquire(&ctx->pool);
        if (c) {
            wire_send_control(c->fd, CTL_END_INCREMENTAL, CTL_END_INCREMENTAL_LEN);
            pool_touch(c);
            pool_release(&ctx->pool, c);
            ctx->ctl_end_sent = 1;
            LOG_INFO("sent ctlEndIncremental");
        } else {
            return 0;
        }
    }

    /* 等待 SERVER_DONE (轮询, 最多 300 秒, 提前终止条件: server_done_rcvd) */
    LOG_INFO("waiting for SERVER_DONE...");
    ctx->server_done_rcvd = 0;
    for (int w = 0; w < 3000 && g_running && !ctx->server_done_rcvd; w++) {
        process_ack(ctx);
        while (flush_one(ctx) > 0) { }
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    if (!ctx->server_done_rcvd) {
        LOG_WARN("SERVER_DONE wait timed out (300s), proceeding anyway");
    }

    ctx->all_done = 1;
    LOG_INFO("allDone set, migration complete");
    return 1;
}

/*
 * should_finish: 判断增量同步是否应该结束。
 *
 * 三种退出条件 (任满足其一即返回 1):
 *   1. 完美收敛 — 所有块已 ACK (unacked == 0)
 *   2. 稳定收敛 — 连续 N 轮无变化块, 剩余未 ACK 块已全部发送完毕
 *      (系统热块不再产生新写入, 只是等待服务端确认)
 *   3. 轮次上限 — 超过 MAX_INCREMENTAL_ROUNDS, 强制结束避免无限循环
 *
 * 返回 1 表示应该结束, 0 表示继续增量同步。
 */
static int should_finish(migrate_ctx_t *ctx, sqlite_db_t *db,
                          int requeued, int current_version) {
    char remote_id[256];
    snprintf(remote_id, sizeof(remote_id), "%s:%d",
             ctx->server_ip, (int)ctx->server_port);
    int unacked = (current_version > 0)
        ? sqlite_count_unacked_v(db, remote_id, current_version)
        : sqlite_count_unacked(db, remote_id);
    int total   = sqlite_count_blocks(db, -1, -1);

    /* 条件 1: 完美收敛 — 零未确认块 */
    if (unacked == 0) {
        LOG_INFO("incremental sync: perfect convergence (0 unacked blocks)");
        return 1;
    }

    /* 条件 2: 稳定收敛 — 连续 N 轮无新变化块, 且大部分块已验证。
     * 仅当 unacked < 5% 总块数时才允许稳定收敛, 避免在 reset_acked
     * 后还有大量未验证块时过早退出。 */
    if (requeued == 0) {
        ctx->zero_change_rounds++;
        if (ctx->zero_change_rounds >= CONVERGENCE_ZERO_ROUNDS) {
            int threshold = total / 20;  /* 5% */
            if (threshold < 512) threshold = 512;  /* 至少一个批次 */
            if (unacked <= threshold) {
                LOG_INFO("incremental sync: converged after %d rounds with 0 "
                         "changes (unacked=%d/%d, will recover via NTFS journal "
                         "replay on target)",
                         ctx->zero_change_rounds, unacked, total);
                return 1;
            }
            LOG_DEBUG("incremental: %d stable rounds but unacked=%d/%d "
                      "(>5%%), continuing verification",
                      ctx->zero_change_rounds, unacked, total);
        }
    } else {
        ctx->zero_change_rounds = 0;  /* 有新变化 → 重置计数器 */
    }

    /* 条件 3: 轮次上限 — 超时强制结束 */
    if (ctx->inc_round >= MAX_INCREMENTAL_ROUNDS) {
        LOG_WARN("incremental sync: max rounds reached (%d), forcing finish "
                 "(unacked=%d, will recover via NTFS journal replay on target)",
                 MAX_INCREMENTAL_ROUNDS, unacked);
        return 1;
    }

    return 0;
}

/* ================================================================
 * 控制台进度显示 (VT100 兼容, Windows 10+)
 * ================================================================ */
static int _pp_lines = 0;  /* 上次打印的行数, 用于光标上移 */

static void cursor_up(int n) {
    if (n <= 0) return;
    static int _vt_ready = 0;
    if (!_vt_ready) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode))
            SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        _vt_ready = 1;
    }
    printf("\x1b[%dA", n);
}
static void cursor_clear_below(void) {
    printf("\x1b[J");
}

/* 增量扫描时每个分区的计数器 */
typedef struct {
    uint64_t scanned;
    uint64_t changed;
    uint64_t skipped;
} part_inc_stat_t;

/*
 * 全量同步分区表 (带进度列), 原地更新。
 */
static void print_full_progress(reader_entry_t *readers, int count,
                                 sqlite_db_t *db) {
    if (_pp_lines > 0)
        cursor_up(_pp_lines);

    printf("\rPartitions to sync:\n");
    printf("\r%-4s %-8s %-10s %-16s %-12s %-14s %-9s %-9s %6s\n",
           "Idx", "Disk", "Part#", "Name", "Blocks", "Source",
           "Sent", "Acked", "%");
    printf("\r---- ---- -------- ---------------- ------------ -------------- --------- -------- ------\n");
    for (int i = 0; i < count; i++) {
        reader_entry_t *e = &readers[i];
        uint64_t acked = db ? (uint64_t)sqlite_count_blocks(db, e->devno, 1) : 0;
        double pct = e->block_count > 0
            ? (double)e->sent_count * 100.0 / (double)e->block_count : 0.0;
        printf("\r%-4d %-8d %-10d %-16s %-12llu %-14s %-9llu %-8llu %5.1f\n",
               i, e->devno, e->part_index, e->name,
               (unsigned long long)e->block_count,
               e->is_vss ? "VSS" : "PhysicalDrive",
               (unsigned long long)e->sent_count,
               (unsigned long long)acked, pct);
    }
    fflush(stdout);
    _pp_lines = count + 3;
}

/*
 * 增量扫描分区表 (带进度列), 原地更新。
 */
static void print_incscan_progress(volume_list_t *vl, const int *active_idx,
                                    int active_count,
                                    const part_inc_stat_t *part_st) {
    if (_pp_lines > 0)
        cursor_up(_pp_lines);

    printf("\rPartitions to sync:\n");
    printf("\r%-4s %-8s %-10s %-16s %-8s %-14s %-9s %-9s %-9s\n",
           "Idx", "Disk", "Part#", "Name", "Blocks", "Source",
           "Scanned", "Changed", "Unchanged");
    printf("\r---- ---- -------- ---------------- -------- -------------- --------- --------- ---------\n");
    for (int i = 0; i < active_count; i++) {
        volume_info_t *v = &vl->volumes[active_idx[i]];
        const char *source = (v->name[0] && v->name[1] == ':') ? "VSS" : "PhysicalDrive";
        printf("\r%-4d %-8d %-10d %-16s %-8llu %-14s %-9llu %-9llu %-9llu\n",
               i, v->devno, v->part_index, v->name,
               (unsigned long long)v->block_count, source,
               (unsigned long long)part_st[i].scanned,
               (unsigned long long)part_st[i].changed,
               (unsigned long long)part_st[i].skipped);
    }
    fflush(stdout);
    _pp_lines = active_count + 3;
}

/* 重置进度行计数 (模式切换时调用) */
static void progress_reset(void) { _pp_lines = 0; }

/*
 * 迁移主循环。
 */
static int do_migrate(migrate_ctx_t *ctx, volume_list_t *vol_list) {
    /* 打开 SQLite 跟踪数据库 */
    sqlite_db_t *db = sqlite_open(ctx->db_path);
    if (!db) {
        LOG_ERROR("cannot open tracking database: %s", ctx->db_path);
        return -1;
    }
    ctx->db = db;  /* 挂到 ctx 上, process_ack 需要持久化 ACK */
    char remote_id[256];
    snprintf(remote_id, sizeof(remote_id), "%s:%d",
             ctx->server_ip, (int)ctx->server_port);
    sqlite_set_remote_id(db, remote_id);

    /* 分配块读取缓冲区 */
    uint8_t *data_buf = malloc((size_t)BLOCK_SIZE);
    if (!data_buf) {
        LOG_ERROR("out of memory for block buffer");
        sqlite_close(db);
        return -1;
    }

    /* ================================================================
     * VSS 快照初始化
     * ================================================================ */
    vss_context_t *vss_ctx = vss_init();
    if (vss_ctx) {
        LOG_INFO("VSS subsystem initialized for consistent backup");
    }

    /* 构建 VSS 卷列表 (仅带盘符的分区) */
    const char *vss_volumes[VSS_MAX_VOLUMES + 1];
    int vss_vol_indices[VSS_MAX_VOLUMES];  /* vol_list 中的索引 */
    int vss_vol_count = 0;

    for (int i = 0; i < vol_list->count && vss_vol_count < VSS_MAX_VOLUMES; i++) {
        volume_info_t *v = &vol_list->volumes[i];
        if (v->name[0] && v->name[1] == ':') {
            vss_volumes[vss_vol_count] = v->name;
            vss_vol_indices[vss_vol_count] = i;
            vss_vol_count++;
        }
    }
    vss_volumes[vss_vol_count] = NULL;

    /* 创建 VSS 快照 */
    vss_snapshot_t snapshots[VSS_MAX_VOLUMES];
    memset(snapshots, 0, sizeof(snapshots));
    int snap_count = 0;

    if (vss_ctx && vss_vol_count > 0) {
        snap_count = vss_create_snapshots(vss_ctx, vss_volumes, snapshots);
        if (snap_count <= 0) {
            LOG_ERROR("VSS: snapshot creation failed (%d snapshots), aborting",
                      snap_count);
            fprintf(stderr, "FATAL: VSS snapshot creation failed, "
                    "cannot guarantee consistent backup\n");
            vss_cleanup(vss_ctx);
            free(data_buf);
            sqlite_close(db);
            return -1;
        }
        LOG_INFO("VSS: %d snapshots created", snap_count);

        /* 将快照路径匹配到 vol_list */
        for (int s = 0; s < snap_count; s++) {
            if (!snapshots[s].valid) continue;

            /* original_volume 格式如 "C:\" — 去掉尾部反斜杠 */
            size_t orig_len = strlen(snapshots[s].original_volume);
            if (orig_len > 0 && snapshots[s].original_volume[orig_len - 1] == '\\')
                orig_len--;

            for (int vi = 0; vi < vss_vol_count; vi++) {
                int vol_idx = vss_vol_indices[vi];
                volume_info_t *vol = &vol_list->volumes[vol_idx];

                if (strncmp(vol->name, snapshots[s].original_volume, orig_len) == 0
                    && vol->name[orig_len] == '\0') {
                    snprintf(vol->vss_path, sizeof(vol->vss_path), "%s",
                             snapshots[s].snapshot_path);
                    vol->has_vss = 1;
                    LOG_INFO("VSS: %s → %s", vol->name, vol->vss_path);
                    break;
                }
            }
        }
    } else {
        LOG_ERROR("VSS: no drive-letter volumes or VSS unavailable, aborting");
        fprintf(stderr, "FATAL: VSS not available, "
                "cannot guarantee consistent backup\n");
        if (vss_ctx) vss_cleanup(vss_ctx);
        free(data_buf);
        sqlite_close(db);
        return -1;
    }

    /* ================================================================
     * 打开读取器 (VSS 快照优先, 回退到 PhysicalDrive)
     * ================================================================ */
    reader_entry_t readers[MAX_VOLUMES];
    int reader_count = 0;
    memset(readers, 0, sizeof(readers));

    for (int i = 0; i < vol_list->count; i++) {
        volume_info_t *vol = &vol_list->volumes[i];

        /* 检查跳过列表 */
        int skip = 0;
        for (int j = 0; j < ctx->skip_count; j++) {
            if (ctx->skip_disks[j] == vol->devno) {
                LOG_INFO("skipping disk %d (in skip list)", vol->devno);
                skip = 1;
                break;
            }
        }
        if (skip) continue;

        const char *reader_path = vol->has_vss ? vol->vss_path : vol->disk_path;
        int use_vss = vol->has_vss;

        block_reader_t *r = block_reader_open(reader_path);
        if (!r && use_vss) {
            /* VSS 读取失败, 回退到 PhysicalDrive */
            LOG_WARN("VSS reader open failed, falling back to %s", vol->disk_path);
            r = block_reader_open(vol->disk_path);
            use_vss = 0;
        }

        if (r) {
            reader_entry_t *entry = &readers[reader_count];
            entry->reader           = r;
            entry->devno            = vol->devno;
            entry->partition_offset = vol->partition_offset;
            entry->block_count      = vol->block_count;
            entry->is_vss           = use_vss;
            entry->live_reader      = NULL;
            snprintf(entry->name, sizeof(entry->name), "%s", vol->name);
            entry->part_index       = vol->part_index;

            /* 打开 PhysicalDrive 读取器用于增量同步 (始终从 live disk 读取)。
             * VSS 快照是静态的时间点副本, 无法检测后续写入变化。 */
            if (ctx->tail_send) {
                entry->live_reader = block_reader_open(vol->disk_path);
                if (!entry->live_reader) {
                    LOG_WARN("cannot open live disk %s for incremental sync, "
                             "using primary reader as fallback", vol->disk_path);
                    entry->live_reader = r;  /* fallback to primary reader */
                }
            }

            reader_count++;

            /* Runtime consistency: VSS snapshot size should match partition.
             * On mismatch, clamp block_count to the smaller of the two. */
            uint64_t reader_blocks = block_reader_block_count(r, (uint32_t)BLOCK_SIZE);
            uint64_t vol_blocks    = vol->block_count;
            if (reader_blocks != vol_blocks) {
                LOG_WARN("partition %s: volume block_count=%llu reader block_count=%llu "
                         "(using smaller)", vol->name,
                         (unsigned long long)vol_blocks,
                         (unsigned long long)reader_blocks);
                if (reader_blocks < vol_blocks) {
                    /* VSS snapshot smaller than expected — clamp to reader size */
                    vol->block_count = reader_blocks;
                }
            }

            LOG_INFO("partition %s: %llu blocks (offset=%llu) via %s%s",
                     vol->name, (unsigned long long)vol->block_count,
                     (unsigned long long)vol->partition_offset,
                     use_vss ? "VSS" : "PhysicalDrive",
                     ctx->tail_send ? " (+ live reader)" : "");
        }
    }

    if (reader_count == 0) {
        LOG_ERROR("no partitions available for migration");
        if (vss_ctx) vss_cleanup(vss_ctx);
        free(data_buf);
        sqlite_close(db);
        return -1;
    }

    /* ================================================================
     * 增量模式: 发送 ctlIncremental
     * ================================================================ */
    if (ctx->tail_send && !ctx->dry_run) {
        pool_conn_t *c = pool_acquire(&ctx->pool);
        if (c) {
            wire_send_control(c->fd, CTL_INCREMENTAL, CTL_INCREMENTAL_LEN);
            pool_release(&ctx->pool, c);
            LOG_INFO("sent ctlIncremental");
        }
    }

    /* ================================================================
     * 分区清单 + 进度表
     * ================================================================ */
    print_full_progress(readers, reader_count, db);

    /* ================================================================
     * 主传输循环
     * ================================================================ */
    LOG_INFO("starting block transfer: %d partitions", reader_count);

    uint64_t sent_blocks  = 0;
    uint64_t total_bytes  = 0;
    ctx->total_blocks = 0;
    progress_reset();

    for (int d = 0; d < reader_count && g_running; d++) {
        reader_entry_t *entry = &readers[d];
        block_reader_t *reader = entry->reader;
        uint64_t part_base = entry->partition_offset;
        uint64_t n_blocks = entry->block_count;

        /* 裁切: block_count 来自分区表, 可能超出物理磁盘边界 */
        uint64_t disk_end = block_reader_size(reader);
        if (disk_end > part_base) {
            uint64_t max_blocks = (disk_end - part_base) / BLOCK_SIZE;
            if (n_blocks > max_blocks) {
                LOG_WARN("partition %d: clamping block count %llu → %llu "
                         "(partition exceeds disk boundary)",
                         d, (unsigned long long)n_blocks,
                         (unsigned long long)max_blocks);
                n_blocks = max_blocks;
            }
        } else {
            LOG_ERROR("partition %d: offset %llu beyond disk end %llu, skipping",
                      d, (unsigned long long)part_base,
                      (unsigned long long)disk_end);
            n_blocks = 0;
        }

        LOG_INFO("partition devno=%d base=%llu: %llu blocks to transfer",
                 entry->devno, (unsigned long long)part_base,
                 (unsigned long long)n_blocks);
        ctx->total_blocks += n_blocks;

        __try {
        for (uint64_t blk = 0; blk < n_blocks && g_running; blk++) {
            uint64_t disk_offset   = part_base + blk * BLOCK_SIZE;
            uint64_t reader_offset = entry->is_vss
                                     ? (blk * BLOCK_SIZE)
                                     : disk_offset;

            /* 背压: 如果队列太深, 等待排空 */
            while (queue_should_backpressure(&ctx->queue) && g_running) {
                process_ack(ctx);
                while (flush_one(ctx) > 0) { }

                if (queue_should_backpressure(&ctx->queue)) {
#ifdef _WIN32
                    Sleep(BACKPRESSURE_SLEEP_MS);
#else
                    usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
                }
            }

            /* 发送一个块 */
            if (send_block(ctx, reader, db, entry->devno,
                          disk_offset, reader_offset, data_buf) == 0) {
                sent_blocks++;
                entry->sent_count++;
                total_bytes += BLOCK_SIZE;
            }

            /* 发送排空 */
            while (flush_one(ctx) > 0) { }

            /* 处理 ACK */
            process_ack(ctx);

            /* 定时器检查 */
            if (blk > 0 && blk % 5000 == 0) {
                LOG_INFO("checkpoint: blk=%llu sent=%llu queue=%d",
                         (unsigned long long)blk,
                         (unsigned long long)sent_blocks,
                         queue_count(&ctx->queue));
            }
            uint64_t now = timer_now_ms();
            timer_event_t ev;
            while ((ev = timer_check(&ctx->timer, now)) != TIMER_NONE) {
                switch (ev) {
                case TIMER_INCREMENTAL:
                    /* 全量传输期间也执行增量轮 — 检测已发送但未 ACK
                     * 的块是否在 live disk 上发生了变化。TailSend 模式下
                     * ACK 不会标记 ack=1，所以这里能捕获变化块。 */
                    if (ctx->tail_send) {
                        ctx->inc_round++;
                        int req = do_incremental_round(ctx, readers, readers,
                                                       reader_count, db, data_buf, 0);
                        LOG_DEBUG("inc-round #%d (in full sync): %d re-sent",
                                  ctx->inc_round, req);
                    }
                    break;
                case TIMER_RETRANSMIT:
                    do_retransmit(ctx, db, readers, reader_count, data_buf);
                    break;
                case TIMER_RECONNECT:
                    if (!ctx->dry_run) do_reconnect(ctx);
                    break;
                case TIMER_ACTION: {
                    print_full_progress(readers, reader_count, ctx->db);
                    LOG_INFO("progress: partition %d/%d block %llu/%llu "
                             "sent=%llu/%llu queue=%d%s",
                             d + 1, reader_count,
                             (unsigned long long)blk,
                             (unsigned long long)n_blocks,
                             (unsigned long long)sent_blocks,
                             (unsigned long long)ctx->total_blocks,
                             queue_count(&ctx->queue),
                             ctx->tail_send ? " [incremental]" : "");
                    break;
                }
                default:
                    break;
                }
                timer_reset(&ctx->timer, ev);
                now = timer_now_ms();
            }
        }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            LOG_ERROR("FATAL: exception 0x%08lx in transfer loop",
                      (unsigned long)GetExceptionCode());
            fprintf(stderr, "FATAL: crash in transfer loop, code=0x%08lx\n",
                    (unsigned long)GetExceptionCode());
            fflush(stderr);
            return -1;
        }

        LOG_INFO("partition devno=%d base=%llu complete: %llu blocks transferred",
                 entry->devno, (unsigned long long)part_base,
                 (unsigned long long)n_blocks);
    }

    /* ================================================================
     * GPT 元数据备份: 前 1MB (主 GPT) + 末尾 2MB (备份 GPT)
     * 对每块物理磁盘各执行一次。
     * ================================================================ */
    {
        int gpt_done[16] = {0};
        for (int d = 0; d < reader_count && g_running; d++) {
            int devno = readers[d].devno;
            if (gpt_done[devno]) continue;
            gpt_done[devno] = 1;

            block_reader_t *disk = readers[d].reader;
            int need_close = 0;
            if (readers[d].is_vss) {
                char disk_path[64];
                snprintf(disk_path, sizeof(disk_path),
                         "\\\\.\\PhysicalDrive%d", devno);
                disk = block_reader_open(disk_path);
                if (!disk) {
                    LOG_WARN("GPT: cannot open PhysicalDrive%d for metadata",
                             devno);
                    continue;
                }
                need_close = 1;
            }

            uint64_t disk_size = block_reader_size(disk);
            /* 对齐到 BLOCK_SIZE，避免 IOCTL_DISK_GET_LENGTH_INFO
             * 返回非整数 MB 导致 receiver 端文件多出尾部碎片 */
            uint64_t aligned_end = (disk_size / BLOCK_SIZE) * BLOCK_SIZE;
            int32_t dno = (int32_t)devno;

            /* 前 1MB — 主 GPT 元数据 */
            if (send_block(ctx, disk, db, dno, 0, 0, data_buf) == 0) {
                sent_blocks++;
                readers[d].sent_count++;
                total_bytes += BLOCK_SIZE;
            }
            while (flush_one(ctx) > 0) { }
            process_ack(ctx);
            LOG_INFO("GPT: primary header sent (devno=%d offset=0)", devno);

            /* 末尾 2MB — 备份 GPT + 对齐间隙 */
            if (aligned_end >= 2 * BLOCK_SIZE) {
                for (int t = 2; t >= 1 && g_running; t--) {
                    uint64_t off = aligned_end - (uint64_t)t * BLOCK_SIZE;
                    if (send_block(ctx, disk, db, dno,
                                  off, off, data_buf) == 0) {
                        sent_blocks++;
                        readers[d].sent_count++;
                        total_bytes += BLOCK_SIZE;
                    }
                    while (flush_one(ctx) > 0) { }
                    process_ack(ctx);
                    LOG_INFO("GPT: backup footer block (devno=%d offset=%llu)",
                             devno, (unsigned long long)off);
                }
            }

            if (need_close) block_reader_close(disk);
        }
    }

    /* ================================================================
     * 排空队列
     * ================================================================ */
    LOG_INFO("draining send queue...");
    int drain_attempts = 0;
    while (queue_count(&ctx->queue) > 0 && drain_attempts < 300) {
        while (flush_one(ctx) > 0) { }
        process_ack(ctx);
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
        drain_attempts++;
    }

    /* ================================================================
     * 增量同步循环 (TailSend=1)
     *
     * 每 18s 从 live disk (PhysicalDrive) 重读所有未 ACK 块,
     * hash 对比, 仅重发变化的块。
     * 当所有块已确认且服务端返回 SERVER_DONE 后退出。
     * ================================================================ */
    if (ctx->tail_send) {
        LOG_INFO("entering incremental sync loop (interval=%d ms, "
                 "max_rounds=%d, convergence_rounds=%d)",
                 RETRANSMIT_TIMER_MS,
                 MAX_INCREMENTAL_ROUNDS, CONVERGENCE_ZERO_ROUNDS);

        /* inc_round 继续计数 (全量同步期间可能已执行若干轮) */
        ctx->zero_change_rounds = 0;

        /* 获取下一个版本号, 记录本轮增量同步开始 */
        ctx->inc_version = sqlite_get_next_version(db, remote_id);
        if (ctx->inc_version < 1) ctx->inc_version = 1;
        {
            int64_t round_start = (int64_t)time(NULL) * 1000;
            sqlite_version_start(db, ctx->inc_version, remote_id, round_start);
            LOG_INFO("incremental: starting version %d", ctx->inc_version);
        }

        while (g_running && !ctx->all_done) {
            /* 发送 + 接收 + 定时器处理 */
            while (flush_one(ctx) > 0) { }
            process_ack(ctx);
            do_reconnect(ctx);

            uint64_t now = timer_now_ms();
            timer_event_t ev;
            while ((ev = timer_check(&ctx->timer, now)) != TIMER_NONE) {
                switch (ev) {
                case TIMER_INCREMENTAL: {
                    ctx->inc_round++;
                    int requeued = do_incremental_round(ctx, readers, readers,
                                                         reader_count, db, data_buf,
                                                         ctx->inc_version);
                    LOG_INFO("incremental round %d/%d (version %d): %d blocks re-sent, "
                             "%d unacked, %d stable rounds",
                             ctx->inc_round, MAX_INCREMENTAL_ROUNDS,
                             ctx->inc_version, requeued,
                             sqlite_count_unacked_v(db, remote_id, ctx->inc_version + 1),
                             ctx->zero_change_rounds);

                    /* 记录本轮结束 */
                    {
                        int64_t round_end = (int64_t)time(NULL) * 1000;
                        sqlite_version_end(db, ctx->inc_version, remote_id,
                                          round_end, 0, requeued);
                    }

                    /* 检查是否应该结束 (完美收敛 / 稳定收敛 / 超时) */
                    if (should_finish(ctx, db, requeued, ctx->inc_version)) {
                        if (timer_cb_real(ctx)) {
                            break;  /* allDone — 退出定时器循环 */
                        }
                    } else {
                        /* 下一轮: 递增版本号, 开始新一轮 */
                        ctx->inc_version++;
                        int64_t next_start = (int64_t)time(NULL) * 1000;
                        sqlite_version_start(db, ctx->inc_version, remote_id,
                                            next_start);
                    }
                    break;
                }
                case TIMER_RETRANSMIT:
                    do_retransmit(ctx, db, readers, reader_count, data_buf);
                    break;
                case TIMER_RECONNECT:
                    if (!ctx->dry_run) do_reconnect(ctx);
                    break;
                case TIMER_ACTION: {
                    print_full_progress(readers, reader_count, db);
                    LOG_INFO("incremental: round %d/%d, %d unacked blocks, "
                             "queue=%d, stable=%d rounds",
                             ctx->inc_round, MAX_INCREMENTAL_ROUNDS,
                             sqlite_count_unacked(db, remote_id),
                             queue_count(&ctx->queue),
                             ctx->zero_change_rounds);
                    break;
                }
                default:
                    break;
                }
                timer_reset(&ctx->timer, ev);

                if (ctx->all_done) break;
                now = timer_now_ms();
            }

            /* allDone 检查 */
            if (ctx->all_done) break;

            /* 短暂休眠避免忙等 (1s) */
#ifdef _WIN32
            Sleep(1000);
#else
            usleep(1000000);
#endif
        }

        /* 迁移完成 — T_BLOCK 保留以便后续 incsync 参考 */
        if (ctx->all_done) {
            LOG_INFO("incremental sync complete after %d rounds (version %d)",
                     ctx->inc_round, ctx->inc_version);
        }
    } else {
        /* 非增量模式: 发送 ctlEndIncremental 结束传输 */
        if (!ctx->dry_run) {
            pool_conn_t *c = pool_acquire(&ctx->pool);
            if (c) {
                wire_send_control(c->fd, CTL_END_INCREMENTAL, CTL_END_INCREMENTAL_LEN);
                pool_touch(c);
                pool_release(&ctx->pool, c);
                LOG_INFO("sent ctlEndIncremental");

                for (int w = 0; w < 300; w++) {
                    process_ack(ctx);
#ifdef _WIN32
                    Sleep(100);
#else
                    usleep(100000);
#endif
                }
            }
        }
    }

    LOG_INFO("migration complete: %llu blocks, %llu bytes",
             (unsigned long long)sent_blocks, (unsigned long long)total_bytes);
    if (_pp_lines > 0) { cursor_clear_below(); progress_reset(); }
    printf("\n");

    /* ================================================================
     * VSS 清理
     * ================================================================ */
    if (vss_ctx) {
        vss_backup_complete(vss_ctx);
        vss_cleanup(vss_ctx);
    }

    /* 清理 */
    for (int i = 0; i < reader_count; i++) {
        block_reader_close(readers[i].reader);
        /* 关闭 live reader (如果不同于 primary reader) */
        if (readers[i].live_reader && readers[i].live_reader != readers[i].reader) {
            block_reader_close(readers[i].live_reader);
        }
    }
    free(data_buf);
    sqlite_close(db);
    return 0;
}

/* ================================================================
 * 子命令: dryrun — 本地模拟全量+增量迁移, 无网络 I/O
 * ================================================================ */

static int cmd_dryrun(int argc, char *argv[]) {
    const char *config_path = (argc >= 3) ? argv[2] : "user.json";
    char *config_json = read_file_all(config_path);

    int log_level    = LOG_LEVEL_INFO;
    int zstd_level   = ZSTD_COMPRESS_LEVEL_MAX;
    int tail_send    = 0;
    int rate_limit_mb = 100;
    char log_path[256] = {0};
    char db_path[512]  = "tracker.db";

    migrate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.dry_run = 1;

    if (config_json) {
        const char *log_section = json_nav(config_json, "Log");
        if (log_section) {
            log_level = json_read_int(log_section, "Level", LOG_LEVEL_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }

        tail_send  = json_read_int(config_json, "TailSend", 0);
        zstd_level = tail_send ? ZSTD_COMPRESS_LEVEL_MIN : ZSTD_COMPRESS_LEVEL_MAX;
        rate_limit_mb = json_read_int(config_json, "RateLimitMB", 100);

        json_read_str(config_json, "DbPath", db_path, sizeof(db_path));

        free(config_json);
    }

    ctx.zstd_level = zstd_level;
    ctx.tail_send  = tail_send;
    ctx.rate_limit_mb     = rate_limit_mb;
    ctx.rate_window_start = 0;
    ctx.rate_bytes_sent   = 0;
    snprintf(ctx.db_path, sizeof(ctx.db_path), "%s", db_path);

    /* 初始化日志 */
    log_init(log_level, log_path[0] ? log_path : NULL);
    LOG_INFO("go2cloud client dry-run starting...");
    LOG_INFO("mode: %s (zstd level %d, no network I/O)",
             tail_send ? "incremental" : "full", zstd_level);

    /* 枚举卷 */
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0) {
        LOG_ERROR("no disks found");
        log_close();
        return 1;
    }

    /* 打印磁盘信息 */
    printf("Dry-run mode -- no server connection needed\n");
    printf("%-6s %-8s %-16s %s\n", "Disk", "Size(GB)", "TotalBlocks", "Name");
    printf("------ ------ ---------------- ----\n");
    for (int i = 0; i < vol_list.count; i++) {
        volume_info_t *v = &vol_list.volumes[i];
        printf("%-6d %-8.2f %-16llu %s\n",
               v->devno,
               (double)v->total_bytes / (1024.0 * 1024.0 * 1024.0),
               (unsigned long long)v->block_count, v->name);
    }
    printf("\n");

    /* 初始化模块 (跳过网络相关) */
    msgpack_writer_init(&ctx.mp_writer);
    queue_init(&ctx.queue);
    timer_init(&ctx.timer);

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 随机数种子 */
    srand((unsigned int)time(NULL));

    time_t start_time = time(NULL);

    int rc = do_migrate(&ctx, &vol_list);

    time_t end_time = time(NULL);
    double elapsed = difftime(end_time, start_time);

    printf("\n");
    printf("========== Dry-Run Results ==========\n");
    printf("  Mode:       %s\n", tail_send ? "incremental (TailSend)" : "full");
    printf("  Status:     %s\n", rc == 0 ? "SUCCESS" : "FAILED");
    printf("  Duration:   %.0f seconds\n", elapsed);
    printf("======================================\n");

    /* 清理 */
    queue_destroy(&ctx.queue);
    msgpack_writer_free(&ctx.mp_writer);
    log_close();

    return rc;
}

/* ================================================================
 * 子命令: incsync — 单轮增量同步 (可反复调用)
 *
 *   client.exe incsync <ip:port> [config.json]
 *
 * 与 TailSend=1 内嵌增量循环不同, incsync 是独立的一次性命令:
 *   1. 连接服务端, 发送 ctlIncremental
 *   2. 重置 ack=1 → ack=0, 从 live disk 重读所有块
 *   3. hash 与 DB 中的旧值对比, 仅发送变化块
 *   4. 发送 ctlEndIncremental, 等待 SERVER_DONE
 *   5. 打印变化块统计, 退出
 *
 * 多轮增量: 反复运行此命令即可。
 *
 * 配置新增字段:
 *   "SkipPartitions": "devno:part,..."  跳过指定分区 (part 从 1 开始)
 *   例: "0:2,0:3,1:1" → 跳过 disk0 分区2/3 和 disk1 分区1
 * ================================================================ */

/* 检查分区是否在跳过列表中。
 * skip_str 格式: "devno:part,devno:part,..."
 * 如 "0:2,0:3" 表示跳过 disk0 part2 和 part3。 */
static int partition_is_skipped(const char *skip_str, int devno, int part_index) {
    if (!skip_str || !skip_str[0]) return 0;

    char target[32];
    snprintf(target, sizeof(target), "%d:%d", devno, part_index);

    const char *p = skip_str;
    while (*p) {
        /* 跳过空白和逗号 */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;

        size_t len = (size_t)(p - start);
        if (len > 0 && len < sizeof(target)
            && strncmp(target, start, len) == 0
            && target[len] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int cmd_incsync(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s incsync <ip:port> [config.json]\n", argv[0]);
        return 1;
    }

    /* 解析 ip:port */
    const char *target = argv[2];
    char server_ip[64] = {0};
    uint16_t server_port = 3389;
    {
        const char *colon = strchr(target, ':');
        if (colon) {
            size_t ip_len = MIN((size_t)(colon - target), sizeof(server_ip) - 1);
            memcpy(server_ip, target, ip_len);
            server_port = (uint16_t)atoi(colon + 1);
        } else {
            strncpy(server_ip, target, sizeof(server_ip) - 1);
        }
    }

    /* 读取配置文件 (可选) */
    const char *config_path = "user.json";
    if (argc >= 4) config_path = argv[3];

    char *config_json = read_file_all(config_path);
    int tail_send = 0;
    int rate_limit_mb = 100;
    char db_path[512] = "tracker.db";
    char skip_part_str[512] = {0};
    int skip_disks[16];
    int skip_count = 0;

    if (config_json) {
        tail_send = json_read_int(config_json, "TailSend", 0);
        rate_limit_mb = json_read_int(config_json, "RateLimitMB", 100);
        json_read_str(config_json, "DbPath", db_path, sizeof(db_path));

        /* 解析 SkipDisks */
        const char *disks_section = json_nav(config_json, "Disks");
        if (!disks_section) {
            const char *sd = strstr(config_json, "\"SkipDisks\"");
            if (sd) {
                sd = strchr(sd, '[');
                if (sd) {
                    sd++;
                    while (*sd && skip_count < 16) {
                        /* 跳过空白和逗号; 遇到 ] 表示数组结束 */
                        while (*sd == ' ' || *sd == ',') sd++;
                        if (*sd == ']' || !*sd) break;
                        if (*sd < '0' || *sd > '9') break;
                        skip_disks[skip_count++] = atoi(sd);
                        while (*sd >= '0' && *sd <= '9') sd++;
                    }
                }
            }
        }

        /* 解析 SkipPartitions (与 Disks section 有无无关) */
        {
            char sp_buf[512];
            if (json_read_str(config_json, "SkipPartitions", sp_buf, sizeof(sp_buf))) {
                snprintf(skip_part_str, sizeof(skip_part_str), "%s", sp_buf);
            }
        }
    }

    /* 初始化日志 */
    int log_level = LOG_LEVEL_INFO;
    char log_path[256] = {0};
    if (config_json) {
        const char *log_section = json_nav(config_json, "Log");
        if (log_section) {
            log_level = json_read_int(log_section, "Level", LOG_LEVEL_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }
    }
    log_init(log_level, log_path[0] ? log_path : NULL);
    LOG_INFO("incsync: starting single-pass incremental sync → %s:%d",
             server_ip, (int)server_port);

    /* 枚举分区 */
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0) {
        LOG_ERROR("incsync: no volumes found");
        free(config_json);
        return 1;
    }
    LOG_INFO("incsync: %d partitions enumerated", vol_list.count);

    /* 过滤分区 */
    int active_count = 0;
    int active_idx[64];
    for (int i = 0; i < vol_list.count; i++) {
        volume_info_t *v = &vol_list.volumes[i];
        int skipped = 0;

        /* 磁盘级别跳过 */
        for (int j = 0; j < skip_count; j++) {
            if (skip_disks[j] == v->devno) { skipped = 1; break; }
        }
        /* 分区级别跳过 */
        if (!skipped && partition_is_skipped(skip_part_str, v->devno, v->part_index)) {
            skipped = 1;
        }

        if (!skipped) {
            active_idx[active_count++] = i;
        }
    }

    if (active_count == 0) {
        LOG_ERROR("incsync: all partitions skipped, nothing to sync");
        free(config_json);
        return 1;
    }

    /* 初始化 Winsock */
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    /* 打开 SQLite */
    sqlite_db_t *db = sqlite_open(db_path);
    if (!db) {
        LOG_ERROR("incsync: cannot open DB %s", db_path);
        free(config_json);
        return 1;
    }
    char remote_id[256];
    snprintf(remote_id, sizeof(remote_id), "%s:%d", server_ip, (int)server_port);
    sqlite_set_remote_id(db, remote_id);

    /* 分配缓冲区 */
    uint8_t *data_buf = malloc((size_t)BLOCK_SIZE);
    if (!data_buf) {
        LOG_ERROR("incsync: out of memory");
        sqlite_close(db);
        free(config_json);
        return 1;
    }

    /* 创建迁移上下文 (复用 flush_one / process_ack 等函数) */
    migrate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.server_ip, server_ip, sizeof(ctx.server_ip) - 1);
    ctx.server_port = server_port;
    ctx.tail_send   = 0;   /* process_ack 中将服务端 ACK 直接标记 ack=1 */
    ctx.dry_run     = 0;
    ctx.db          = db;
    ctx.zstd_level  = tail_send ? 1 : 7;
    ctx.rate_limit_mb     = rate_limit_mb;
    ctx.rate_window_start = 0;
    ctx.rate_bytes_sent   = 0;

    queue_init(&ctx.queue);
    msgpack_writer_init(&ctx.mp_writer);

    /* 连接服务端 (单连接) */
    if (pool_init(&ctx.pool, ctx.server_ip, ctx.server_port) < 0) {
        LOG_ERROR("incsync: cannot connect to %s:%d", server_ip, (int)server_port);
        sqlite_close(db);
        free(data_buf);
        free(config_json);
        return 1;
    }

    /* 发送 ctlIncremental 通知服务端进入增量模式 */
    {
        pool_conn_t *c = pool_acquire(&ctx.pool);
        if (c) {
            wire_send_control(c->fd, CTL_INCREMENTAL, CTL_INCREMENTAL_LEN);
            pool_release(&ctx.pool, c);
            LOG_INFO("incsync: sent ctlIncremental");
        }
    }

    /* ================================================================
     * VSS 快照初始化
     * ================================================================ */
    vss_context_t *vss_ctx = vss_init();  /* VSS_CTX_BACKUP, 非持久快照 */
    if (vss_ctx) LOG_INFO("incsync: VSS subsystem initialized");

    /* 构建 VSS 卷列表 (仅分区有盘符的) */
    const char *vss_volumes[VSS_MAX_VOLUMES + 1];
    int vss_vol_indices[VSS_MAX_VOLUMES];
    int vss_vol_count = 0;
    for (int i = 0; i < vol_list.count && vss_vol_count < VSS_MAX_VOLUMES; i++) {
        volume_info_t *v = &vol_list.volumes[i];
        if (v->name[0] && v->name[1] == ':') {
            vss_volumes[vss_vol_count] = v->name;
            vss_vol_indices[vss_vol_count] = i;
            vss_vol_count++;
        }
    }
    vss_volumes[vss_vol_count] = NULL;

    /* 创建快照并映射到 vol_list */
    if (vss_ctx && vss_vol_count > 0) {
        vss_snapshot_t snapshots[VSS_MAX_VOLUMES];
        memset(snapshots, 0, sizeof(snapshots));
        int snap_count = vss_create_snapshots(vss_ctx, vss_volumes, snapshots);
        if (snap_count > 0) {
            LOG_INFO("incsync: %d VSS snapshots created", snap_count);
            for (int s = 0; s < snap_count; s++) {
                if (!snapshots[s].valid) continue;
                size_t orig_len = strlen(snapshots[s].original_volume);
                if (orig_len > 0 && snapshots[s].original_volume[orig_len-1] == '\\')
                    orig_len--;
                for (int vi = 0; vi < vss_vol_count; vi++) {
                    int vol_idx = vss_vol_indices[vi];
                    volume_info_t *vol = &vol_list.volumes[vol_idx];
                    if (strncmp(vol->name, snapshots[s].original_volume, orig_len) == 0
                        && vol->name[orig_len] == '\0') {
                        snprintf(vol->vss_path, sizeof(vol->vss_path), "%s",
                                 snapshots[s].snapshot_path);
                        vol->has_vss = 1;
                        LOG_INFO("incsync VSS: %s -> %s", vol->name, vol->vss_path);
                        break;
                    }
                }
            }
        } else {
            LOG_WARN("incsync: VSS snapshot creation failed (%d), "
                     "will use PhysicalDrive fallback", snap_count);
        }
    }

    /* ================================================================
     * 获取下一版本号, 开始本轮增量同步
     * ================================================================ */
    int current_version = sqlite_get_next_version(db, remote_id);
    if (current_version < 1) current_version = 1;
    {
        int64_t round_start = (int64_t)time(NULL) * 1000;
        sqlite_version_start(db, current_version, remote_id, round_start);
        LOG_INFO("incsync: starting version %d", current_version);
    }

    /* ================================================================
     * 核心: 逐分区逐块扫描, hash 对比, 仅发送变化块
     * ================================================================ */
    int64_t now_ms = (int64_t)timer_now_ms();
    uint64_t total_blocks    = 0;
    uint64_t blocks_scanned = 0;
    uint64_t blocks_changed = 0;
    uint64_t blocks_skipped  = 0;
    uint64_t bytes_sent      = 0;

    /* 每个活跃分区的计数器 */
    part_inc_stat_t part_st[64];
    memset(part_st, 0, sizeof(part_st));
    progress_reset();

    /* 首次打印分区进度表 */
    print_incscan_progress(&vol_list, active_idx, active_count, part_st);

    /* 计算总块数 (仅活跃分区) */
    for (int ai = 0; ai < active_count; ai++) {
        total_blocks += vol_list.volumes[active_idx[ai]].block_count;
    }

    for (int ai = 0; ai < active_count && g_running; ai++) {
        volume_info_t *vol = &vol_list.volumes[active_idx[ai]];
        LOG_INFO("incsync: scanning %s (devno=%d part=%d) %llu blocks...",
                 vol->name, vol->devno, vol->part_index,
                 (unsigned long long)vol->block_count);

        const char *reader_path = vol->has_vss ? vol->vss_path : vol->disk_path;
        block_reader_t *reader = block_reader_open(reader_path);
        if (!reader && vol->has_vss) {
            LOG_WARN("incsync: VSS reader open failed for %s, falling back to %s",
                     vol->name, vol->disk_path);
            reader = block_reader_open(vol->disk_path);
            vol->has_vss = 0;
        }
        if (!reader) {
            LOG_ERROR("incsync: cannot open %s, skipping", reader_path);
            continue;
        }

        uint64_t part_base = vol->partition_offset;
        uint64_t n_blocks  = vol->block_count;

        for (uint64_t blk = 0; blk < n_blocks && g_running; blk++) {
            uint64_t disk_offset = part_base + blk * BLOCK_SIZE;
            uint32_t data_len = 0;
            uint64_t reader_offset = vol->has_vss
                ? (blk * BLOCK_SIZE) : disk_offset;

            if (block_reader_read(reader, reader_offset, data_buf,
                                  (uint32_t)BLOCK_SIZE, &data_len) != 0) {
                LOG_WARN("incsync: read error devno=%d offset=%llu",
                         vol->devno, (unsigned long long)disk_offset);
                continue;
            }
            if (data_len == 0) continue;

            blocks_scanned++;
            part_st[ai].scanned++;
            uint64_t new_hash = hash_block(data_buf, data_len,
                                           disk_offset & 0xFFFFFFFF);

            /* 对比 DB 中的 hash */
            uint64_t stored_hash = 0;
            int lookup_rc = sqlite_block_lookup(db, vol->devno,
                                                (int64_t)disk_offset,
                                                &stored_hash);
            if (lookup_rc == 0 && stored_hash == new_hash) {
                /* 块未变化 — 标记为本轮已验证 */
                blocks_skipped++;
                part_st[ai].skipped++;
                sqlite_update_last_sent(db, vol->devno,
                                        (int64_t)disk_offset, now_ms);
                sqlite_block_mark_verified(db, vol->devno,
                                           (int64_t)disk_offset,
                                           current_version);
            } else {
                /* 块变化或新增 → MsgPack 编码 → 入队 */
                LOG_INFO("incsync: changed block devno=%d offset=%llu "
                         "hash=0x%016llx",
                         vol->devno, (unsigned long long)disk_offset,
                         (unsigned long long)new_hash);
                if (msgpack_encode_block(&ctx.mp_writer, vol->devno,
                                         (int64_t)disk_offset,
                                         data_buf, data_len) != 0) {
                    LOG_WARN("incsync: encode failed devno=%d offset=%llu",
                             vol->devno, (unsigned long long)disk_offset);
                    continue;
                }

                int rc;
                while ((rc = queue_push(&ctx.queue, vol->devno,
                                        (int64_t)disk_offset, new_hash,
                                        ctx.mp_writer.buf,
                                        ctx.mp_writer.written)) != 0) {
                    while (flush_one(&ctx) > 0) { }
                    process_ack(&ctx);
#ifdef _WIN32
                    Sleep(50);
#else
                    usleep(50000);
#endif
                }

                sqlite_block_upsert_v(db, vol->devno, (int64_t)disk_offset,
                                      (int32_t)data_len, new_hash, 0,
                                      current_version);
                blocks_changed++;
                part_st[ai].changed++;
                bytes_sent += data_len;
            }

            /* 周期性 flush + ACK 处理 (每 1024 块) */
            if ((blocks_scanned & 1023) == 0) {
                while (flush_one(&ctx) > 0) { }
                process_ack(&ctx);
                print_incscan_progress(&vol_list, active_idx, active_count,
                                       part_st);
                LOG_INFO("incsync: %llu scanned, %llu changed, %llu skipped",
                         (unsigned long long)blocks_scanned,
                         (unsigned long long)blocks_changed,
                         (unsigned long long)blocks_skipped);
            }
        }

        block_reader_close(reader);
    }

    /* VSS 清理: 读取完毕即可释放快照 */
    if (vss_ctx) {
        vss_backup_complete(vss_ctx);
        vss_cleanup(vss_ctx);
        vss_ctx = NULL;
        LOG_INFO("incsync: VSS snapshots released");
    }

    /* ================================================================
     * 排空队列 + 收尾 ACK
     * ================================================================ */
    LOG_INFO("incsync: draining queue (%d pending)...", queue_count(&ctx.queue));
    for (int drain = 0; drain < 300; drain++) {
        while (flush_one(&ctx) > 0) { }
        process_ack(&ctx);
        if (queue_count(&ctx.queue) == 0) break;
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    /* 发送 ctlEndIncremental + 等待 SERVER_DONE */
    {
        pool_conn_t *c = pool_acquire(&ctx.pool);
        if (c) {
            wire_send_control(c->fd, CTL_END_INCREMENTAL, CTL_END_INCREMENTAL_LEN);
            pool_release(&ctx.pool, c);
            LOG_INFO("incsync: sent ctlEndIncremental, waiting for SERVER_DONE...");
        }
    }

    for (int w = 0; w < 300; w++) {
        process_ack(&ctx);
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    /* 记录本轮结束 */
    {
        int64_t round_end = (int64_t)time(NULL) * 1000;
        sqlite_version_end(db, current_version, remote_id, round_end,
                           (int)blocks_scanned, (int)blocks_changed);
        LOG_INFO("incsync: version %d ended, scanned=%d changed=%d",
                 current_version, (int)blocks_scanned, (int)blocks_changed);
    }

    /* ================================================================
     * 结果输出
     * ================================================================ */
    if (_pp_lines > 0) { cursor_clear_below(); progress_reset(); }
    printf("\n");
    printf("========== IncSync Results ==========\n");
    printf("  Version:  %d\n", current_version);
    printf("  Scanned:  %llu blocks\n",   (unsigned long long)blocks_scanned);
    printf("  Skipped:  %llu (unchanged)\n", (unsigned long long)blocks_skipped);
    printf("  Changed:  %llu blocks\n",   (unsigned long long)blocks_changed);
    printf("  Sent:     %llu bytes (%.2f GB)\n",
           (unsigned long long)bytes_sent,
           (double)bytes_sent / (1024.0 * 1024.0 * 1024.0));
    printf("======================================\n");

    LOG_INFO("incsync: complete — %llu scanned, %llu changed, %llu bytes sent",
             (unsigned long long)blocks_scanned,
             (unsigned long long)blocks_changed,
             (unsigned long long)bytes_sent);

    /* 清理 */
    queue_destroy(&ctx.queue);
    msgpack_writer_free(&ctx.mp_writer);
    pool_destroy(&ctx.pool);
    sqlite_close(db);
    free(data_buf);
    free(config_json);
    log_close();

    return 0;
}

/* ================================================================
 * 入口
 * ================================================================ */

int main(int argc, char *argv[]) {
    /* 无参数: 显示帮助 */
    if (argc < 2) {
        printf("go2cloud client — block-level migration transfer\n");
        printf("Usage:\n");
        printf("  %s <ip:port> [config.json]    Run migration\n", argv[0]);
        printf("  %s info                       Show disk information\n", argv[0]);
        printf("  %s hash <file>                Compute block hash\n", argv[0]);
        printf("  %s check <disk>               Check disk accessibility\n", argv[0]);
        printf("  %s isbios                     Detect firmware type (UEFI/Legacy)\n", argv[0]);
        printf("  %s begin_session              Mark migration session start\n", argv[0]);
        printf("  %s end_session                Clean block tracking database\n", argv[0]);
        printf("  %s sentbytes                  Print total confirmed bytes\n", argv[0]);
        printf("  %s test_vss                   Test VSS snapshot functionality\n", argv[0]);
        printf("  %s vss_query                  List all existing snapshots\n", argv[0]);
        printf("  %s vss_delete <guid>          Delete specific snapshot\n", argv[0]);
        printf("  %s vss_delete --all           Delete all snapshots\n", argv[0]);
        printf("  %s blockinfo [flags] [db] [devno] [offset]  Query block tracking DB\n", argv[0]);
        printf("        --pending           Show only pending (ack=0) blocks\n");
        printf("        --acked             Show only confirmed (ack=1) blocks\n");
        printf("  %s dryrun [config.json]       Simulate full migration locally\n", argv[0]);
        printf("  %s incsync <ip:port> [config.json]  Single-pass incremental sync\n", argv[0]);
        printf("  %s --help                     Show this help\n", argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        printf("go2cloud client — block-level migration transfer\n");
        printf("Usage:\n");
        printf("  %s <ip:port> [config.json]\n", argv[0]);
        printf("  %s info\n", argv[0]);
        printf("  %s hash <file>\n", argv[0]);
        printf("  %s check <disk>\n", argv[0]);
        printf("  %s isbios\n", argv[0]);
        printf("  %s begin_session\n", argv[0]);
        printf("  %s end_session\n", argv[0]);
        printf("  %s sentbytes\n", argv[0]);
        printf("  %s test_vss\n", argv[0]);
        printf("  %s vss_query\n", argv[0]);
        printf("  %s vss_delete <guid>\n", argv[0]);
        printf("  %s vss_delete --all\n", argv[0]);
        printf("  %s dryrun [config.json]\n", argv[0]);
        printf("  %s incsync <ip:port> [config.json]\n", argv[0]);
        printf("  %s blockinfo [flags] [db] [devno] [offset]\n", argv[0]);
        printf("\nConfig file defaults to user.json\n");
        return 0;
    }

    /* Init log early so subcommands can use LOG_* macros safely */
    log_init(LOG_LEVEL_INFO, NULL);

    /* 子命令 */
    if (strcmp(argv[1], "info") == 0) {
        return cmd_info();
    }
    if (strcmp(argv[1], "hash") == 0 && argc >= 3) {
        return cmd_hash(argv[2]);
    }
    if (strcmp(argv[1], "check") == 0 && argc >= 3) {
        return cmd_check(argv[2]);
    }
    if (strcmp(argv[1], "begin_session") == 0) {
        return cmd_begin_session();
    }
    if (strcmp(argv[1], "isbios") == 0) {
        return cmd_isbios();
    }
    if (strcmp(argv[1], "end_session") == 0) {
        return cmd_end_session("tracker.db");
    }
    if (strcmp(argv[1], "sentbytes") == 0) {
        return cmd_sentbytes("tracker.db");
    }
    if (strcmp(argv[1], "test_vss") == 0) {
        return cmd_test_vss();
    }
    if (strcmp(argv[1], "vss_query") == 0) {
        return cmd_vss_query();
    }
    if (strcmp(argv[1], "vss_delete") == 0) {
        return cmd_vss_delete(argc, argv);
    }
    if (strcmp(argv[1], "blockinfo") == 0) {
        return cmd_blockinfo(argc, argv);
    }
    if (strcmp(argv[1], "dryrun") == 0) {
        return cmd_dryrun(argc, argv);
    }
    if (strcmp(argv[1], "incsync") == 0) {
        return cmd_incsync(argc, argv);
    }

    /* ————— 迁移模式 ————— */

    /* 解析 ip:port */
    const char *target = argv[1];
    char server_ip[64] = {0};
    uint16_t server_port = 3389;
    {
        const char *colon = strchr(target, ':');
        if (colon) {
            size_t ip_len = MIN((size_t)(colon - target), sizeof(server_ip) - 1);
            memcpy(server_ip, target, ip_len);
            server_port = (uint16_t)atoi(colon + 1);
        } else {
            strncpy(server_ip, target, sizeof(server_ip) - 1);
        }
        if (server_port == 0) server_port = 3389;
    }

    /* 读取配置文件 */
    const char *config_path = (argc >= 3) ? argv[2] : "user.json";
    char *config_json = read_file_all(config_path);

    /* 解析配置 */
    int log_level    = LOG_LEVEL_INFO;
    int zstd_level   = ZSTD_COMPRESS_LEVEL_MAX;  /* full migration: level 7 */
    int tail_send    = 0;
    int rate_limit_mb = 100;
    char log_path[256] = {0};
    char db_path[512]  = "tracker.db";

    migrate_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    strncpy(ctx.server_ip, server_ip, sizeof(ctx.server_ip) - 1);
    ctx.server_port = server_port;

    if (config_json) {
        /* 日志配置 */
        const char *log_section = json_nav(config_json, "Log");
        if (log_section) {
            log_level = json_read_int(log_section, "Level", LOG_LEVEL_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }

        /* 迁移参数 */
        tail_send  = json_read_int(config_json, "TailSend", 0);
        rate_limit_mb = json_read_int(config_json, "RateLimitMB", 100);
        zstd_level = tail_send ? ZSTD_COMPRESS_LEVEL_MIN : ZSTD_COMPRESS_LEVEL_MAX;

        /* 数据库路径 */
        json_read_str(config_json, "DbPath", db_path, sizeof(db_path));

        /* 跳过磁盘列表 (简化: 仅支持整数数组, 需要手动解析) */
        /* (生产环境使用完整的 JSON 解析库) */

        free(config_json);
    }

    ctx.zstd_level = zstd_level;
    ctx.tail_send  = tail_send;
    ctx.rate_limit_mb     = rate_limit_mb;
    ctx.rate_window_start = 0;
    ctx.rate_bytes_sent   = 0;
    snprintf(ctx.db_path, sizeof(ctx.db_path), "%s", db_path);

    /* 初始化日志 */
    log_init(log_level, log_path[0] ? log_path : NULL);
    LOG_INFO("go2cloud client starting...");
    LOG_INFO("target: %s:%d (zstd level %d, %s mode)",
             server_ip, (int)server_port, zstd_level,
             tail_send ? "incremental" : "full");

    /* 初始化 Winsock (Windows) */
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    /* 枚举卷 */
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0) {
        LOG_ERROR("no disks found");
        log_close();
        return 1;
    }

    /* 初始化模块 */
    msgpack_writer_init(&ctx.mp_writer);

    if (pool_init(&ctx.pool, ctx.server_ip, ctx.server_port) != 0) {
        LOG_ERROR("cannot connect to %s:%d", ctx.server_ip, (int)ctx.server_port);
        msgpack_writer_free(&ctx.mp_writer);
        log_close();
        return 1;
    }

    queue_init(&ctx.queue);
    timer_init(&ctx.timer);

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 随机数种子 (用于增量同步 Fisher-Yates 洗牌) */
    srand((unsigned int)time(NULL));

    /* 执行迁移 */
    int rc = do_migrate(&ctx, &vol_list);

    /* 清理 */
    LOG_INFO("cleaning up...");
    pool_destroy(&ctx.pool);
    queue_destroy(&ctx.queue);
    msgpack_writer_free(&ctx.mp_writer);
    log_close();

#ifdef _WIN32
    WSACleanup();
#endif

    return rc;
}
