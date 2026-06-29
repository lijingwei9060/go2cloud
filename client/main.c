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
#define SOCKET_ERRNO  WSAGetLastError()
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
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), '{');
    return p;
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
    socket_pool_t pool;              /* 连接池 */
    send_queue_t  queue;             /* 发送队列 */
    timer_mgr_t   timer;             /* 定时器 */
    msgpack_writer_t mp_writer;     /* MsgPack 编码器 */
} migrate_ctx_t;

/* ================================================================
 * 子命令: info — 显示磁盘信息
 * ================================================================ */

static int cmd_info(void) {
    volume_list_t vol_list;
    if (volume_enumerate(&vol_list) != 0) {
        fprintf(stderr, "No fixed disks found or access denied.\n");
        return 1;
    }

    printf("Disk Information:\n");
    printf("%-6s %-8s %-16s %s\n", "DevNo", "Size(GB)", "TotalBlocks(1MB)", "Path");
    printf("------ ------ ---------------- ----\n");

    for (int i = 0; i < vol_list.count; i++) {
        volume_info_t *v = &vol_list.volumes[i];
        printf("%-6d %-8.2f %-16llu %s\n",
               v->devno,
               (double)v->total_bytes / (1024.0 * 1024.0 * 1024.0),
               (unsigned long long)v->block_count,
               v->path);
    }
    return 0;
}

/* ================================================================
 * 子命令: hash <file> — 计算文件哈希
 * ================================================================ */

static int cmd_hash(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", filepath);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fprintf(stderr, "Empty or invalid file: %s\n", filepath);
        fclose(f);
        return 1;
    }

    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fprintf(stderr, "Out of memory\n");
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
        fprintf(stderr, "Cannot open disk: %s\n", disk_path);
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
                /* 从队列标记确认 */
                queue_ack(&ctx->queue, resp.devno, resp.offset);
            } else if (resp.type == RESPONSE_SERVER_DONE) {
                LOG_INFO("SERVER_DONE: incremental round complete");
                /* 增量轮次结束 — 可以开始下一轮 */
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
 */
static int send_block(migrate_ctx_t *ctx, block_reader_t *reader,
                      sqlite_db_t *db, int32_t devno, uint64_t offset,
                      uint8_t *data_buf) {

    uint32_t data_len = 0;
    if (block_reader_read(reader, offset, data_buf,
                          (uint32_t)BLOCK_SIZE, &data_len) != 0) {
        LOG_ERROR("read block failed: devno=%d offset=%llu",
                  devno, (unsigned long long)offset);
        return -1;
    }

    if (data_len == 0) {
        return 0; /* EOF */
    }

    /* 计算哈希 */
    uint64_t h = hash_block(data_buf, data_len, offset & 0xFFFFFFFF);

    /* 增量去重: 查询 SQLite */
    if (ctx->zstd_level > 0) {  /* 仅增量模式下查询 */
        uint64_t stored_hash = 0;
        if (sqlite_block_lookup(db, devno, (int64_t)offset, &stored_hash) == 0) {
            if (stored_hash == h) {
                LOG_DEBUG("skip: devno=%d offset=%llu (hash unchanged 0x%016llx)",
                          devno, (unsigned long long)offset,
                          (unsigned long long)h);
                return 0; /* 跳过未变化的块 */
            }
        }
    }

    /* MsgPack 编码 */
    if (msgpack_encode_block(&ctx->mp_writer, devno, (int64_t)offset,
                              data_buf, data_len) != 0) {
        LOG_ERROR("msgpack encode failed: devno=%d offset=%llu",
                  devno, (unsigned long long)offset);
        return -1;
    }

    /* 入队 */
    int rc = queue_push(&ctx->queue, devno, (int64_t)offset, h,
                        ctx->mp_writer.buf, ctx->mp_writer.written);
    if (rc == -1) {
        LOG_WARN("queue full, block %d:%llu dropped", devno,
                 (unsigned long long)offset);
        return -1;
    }
    if (rc == 2) {
        /* 背压 — 等待队列排空 */
        LOG_DEBUG("backpressure at block %d:%llu", devno,
                  (unsigned long long)offset);
#ifdef _WIN32
        Sleep(BACKPRESSURE_SLEEP_MS);
#else
        usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
    }

    /* 更新 SQLite */
    sqlite_block_upsert(db, devno, (int64_t)offset,
                        (int32_t)data_len, h, 0);

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
    return 1;
}

/*
 * 处理重传: 扫描 SQLite 中未 ACK 的块, 和队列中待发送的块,
 * 对超过最小间隔的块进行重传。
 */
static void do_retransmit(migrate_ctx_t *ctx, sqlite_db_t *db,
                          block_reader_t *reader, uint8_t *data_buf) {
    int32_t  devnos[256];
    int64_t  offsets[256];
    int count = sqlite_get_unacked(db, devnos, offsets, 256, "");

    LOG_DEBUG("retransmit scan: %d unacked blocks", count);

    for (int i = 0; i < count; i++) {
        if (!g_running) break;

        /* 检查是否已在队列中 */
        int in_queue = 0;
        queue_entry_t entry;
        /* (简化: 不检查队列 — 已 ACK 的块在 queue_ack 中标记 pending=0,
         *         未 ACK 且不在队列中的块需要重新读取并重传) */

        LOG_INFO("retransmit: devno=%d offset=%lld",
                 devnos[i], (long long)offsets[i]);
        send_block(ctx, reader, db, devnos[i], (uint64_t)offsets[i], data_buf);
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
 * 迁移主循环。
 */
static int do_migrate(migrate_ctx_t *ctx, volume_list_t *vol_list) {
    /* 打开 SQLite 跟踪数据库 */
    sqlite_db_t *db = sqlite_open(ctx->db_path);
    if (!db) {
        LOG_ERROR("cannot open tracking database: %s", ctx->db_path);
        return -1;
    }
    /* 使用服务端 IP:port 作为 remote_id */
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

    /* 为每个磁盘打开读取器 */
    block_reader_t *readers[MAX_VOLUMES] = { NULL };
    int reader_count = 0;

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

        readers[reader_count] = block_reader_open(vol->path);
        if (readers[reader_count]) {
            reader_count++;
        }
    }

    if (reader_count == 0) {
        LOG_ERROR("no disks available for migration");
        free(data_buf);
        sqlite_close(db);
        return -1;
    }

    /* 增量模式: 发送 ctlIncremental */
    if (ctx->zstd_level > 1) {
        pool_conn_t *c = pool_acquire(&ctx->pool);
        if (c) {
            wire_send_control(c->fd, CTL_INCREMENTAL, CTL_INCREMENTAL_LEN);
            pool_release(&ctx->pool, c);
            LOG_INFO("sent ctlIncremental");
        }
    }

    /* 主传输循环: 遍历所有磁盘的所有块 */
    LOG_INFO("starting block transfer: %d disks", reader_count);

    uint64_t total_blocks = 0;
    uint64_t sent_blocks  = 0;
    uint64_t total_bytes  = 0;

    for (int d = 0; d < reader_count && g_running; d++) {
        block_reader_t *reader = readers[d];
        volume_info_t *vol = &vol_list->volumes[d];
        uint64_t n_blocks = block_reader_block_count(reader, (uint32_t)BLOCK_SIZE);

        LOG_INFO("disk %d (%s): %llu blocks to transfer",
                 vol->devno, vol->path, (unsigned long long)n_blocks);
        total_blocks += n_blocks;

        for (uint64_t blk = 0; blk < n_blocks && g_running; blk++) {
            uint64_t offset = blk * BLOCK_SIZE;

            /* 背压: 如果队列太深, 等待排空 */
            while (queue_should_backpressure(&ctx->queue) && g_running) {
                process_ack(ctx);
                while (flush_one(ctx) > 0) { /* 发送尽可能多 */ }

                if (queue_should_backpressure(&ctx->queue)) {
#ifdef _WIN32
                    Sleep(BACKPRESSURE_SLEEP_MS);
#else
                    usleep(BACKPRESSURE_SLEEP_MS * 1000);
#endif
                }
            }

            /* 发送一个块 */
            if (send_block(ctx, reader, db,
                          vol->devno, offset, data_buf) == 0) {
                sent_blocks++;
                total_bytes += BLOCK_SIZE;
            }

            /* 发送排空: 尽量从队列发送 */
            while (flush_one(ctx) > 0) { }

            /* 处理 ACK */
            process_ack(ctx);

            /* 定时器检查 */
            uint64_t now = timer_now_ms();
            timer_event_t ev;
            while ((ev = timer_check(&ctx->timer, now)) != TIMER_NONE) {
                switch (ev) {
                case TIMER_RETRANSMIT:
                    do_retransmit(ctx, db, readers[d], data_buf);
                    break;
                case TIMER_RECONNECT:
                    do_reconnect(ctx);
                    break;
                case TIMER_ACTION:
                    LOG_INFO("progress: disk %d/%d block %llu/%llu sent=%llu/%llu queue=%d",
                             d + 1, reader_count, (unsigned long long)blk,
                             (unsigned long long)n_blocks,
                             (unsigned long long)sent_blocks,
                             (unsigned long long)total_blocks,
                             queue_count(&ctx->queue));
                    break;
                default:
                    break;
                }
                timer_reset(&ctx->timer, ev);
                now = timer_now_ms();
            }
        }

        LOG_INFO("disk %d complete: %llu blocks transferred",
                 vol->devno, (unsigned long long)n_blocks);
    }

    /* 排空队列 */
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

    /* 增量模式: 发送 ctlEndIncremental, 等待 SERVER_DONE */
    if (ctx->zstd_level > 1) {
        pool_conn_t *c = pool_acquire(&ctx->pool);
        if (c) {
            wire_send_control(c->fd, CTL_END_INCREMENTAL, CTL_END_INCREMENTAL_LEN);
            pool_touch(c);
            pool_release(&ctx->pool, c);
            LOG_INFO("sent ctlEndIncremental");

            /* 等待 SERVER_DONE (最多 30 秒) */
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

    LOG_INFO("migration complete: %llu blocks, %llu bytes",
             (unsigned long long)sent_blocks, (unsigned long long)total_bytes);

    /* 清理 */
    for (int i = 0; i < reader_count; i++) {
        block_reader_close(readers[i]);
    }
    free(data_buf);
    sqlite_close(db);
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
        printf("  %s check <disk>               Check disk accessibility\n");
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
        printf("\nConfig file defaults to user.json\n");
        return 0;
    }

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
    int log_level    = LOG_INFO;
    int zstd_level   = ZSTD_COMPRESS_LEVEL_MAX;  /* 默认全量: 级别 7 */
    int tail_send    = 0;
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
            log_level = json_read_int(log_section, "Level", LOG_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }

        /* 迁移参数 */
        tail_send  = json_read_int(config_json, "TailSend", 0);
        zstd_level = tail_send ? ZSTD_COMPRESS_LEVEL_MIN : ZSTD_COMPRESS_LEVEL_MAX;

        /* 数据库路径 */
        json_read_str(config_json, "DbPath", db_path, sizeof(db_path));

        /* 跳过磁盘列表 (简化: 仅支持整数数组, 需要手动解析) */
        /* (生产环境使用完整的 JSON 解析库) */

        free(config_json);
    }

    ctx.zstd_level = zstd_level;
    ctx.tail_send  = tail_send;
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
