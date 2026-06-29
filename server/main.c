/*
 * main.c — 服务端 (Receiver) 入口
 *
 * go2cloud 块级迁移服务端, 负责:
 *   1. TCP 监听客户端连接
 *   2. 解码有线协议 (TCP 帧 → "abc" → Zstd → MsgPack)
 *   3. 将块数据写入目标磁盘
 *   4. 返回 ACK 响应
 *   5. 处理增量协议 (ctlIncremental / ctlEndIncremental)
 *
 * 用法:
 *   receiver --config receiver.json
 *
 * 配置文件格式 (receiver.json):
 *   {
 *     "Listen": {"Address": "0.0.0.0", "Port": 3389, "MaxConnections": 7},
 *     "Target": {"Disks": {"0": "/dev/sdb", "1": "/dev/sdc"}},
 *     "Log": {"Level": "info", "Path": "/var/log/receiver.log"}
 *   }
 *
 * 编译 (Linux):
 *   gcc -O2 -Wall -o receiver server/*.c -lzstd -lpthread
 *
 * 编译 (Windows):
 *   cl /O2 /Fe:receiver.exe server\*.c /Iinclude /link libzstd.lib ws2_32.lib
 */

#include "log.h"
#include "session.h"
#include "block_writer.h"
#include "protocol_decoder.h"
#include "ack.h"
#include "../include/protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define SOCKET_INVALID  INVALID_SOCKET
#define SOCKET_ERRNO    WSAGetLastError()
#define CLOSE_SOCKET(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
typedef int socket_t;
#define SOCKET_INVALID  (-1)
#define SOCKET_ERRNO    errno
#define CLOSE_SOCKET(s) close(s)
#endif

/* ================================================================
 * 简易 JSON 解析 (仅读取本配置所需字段, 避免引入第三方库依赖)
 * ================================================================ */

#define JSON_MAX_DEPTH   16
#define JSON_MAX_KEY     64
#define JSON_MAX_STR     512

/* 从 JSON 字符串中读取顶层整数字段 */
static int json_read_int(const char *json, const char *key, int default_val) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return default_val;
    p = strchr(p + strlen(search), ':');
    if (!p) return default_val;
    /* 跳过 : 和空白 */
    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
    return atoi(p);
}

/* 从 JSON 字符串中读取顶层字符串字段 */
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

/* 读取嵌套路径: "A"."B" → 返回 B 下的字段值 */
static const char *json_nav(const char *json, const char *parent, const char *child) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", parent);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    /* 在 parent 的花括号范围内找 child */
    p = strchr(p + strlen(search), '{');
    if (!p) return NULL;
    return p;  /* 返回 { 的位置, 用 json_read_* 在此范围内查找 */
}

/* 读取文件全部内容 */
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
 * 信号处理
 * ================================================================ */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("received signal %d, shutting down...", sig);
        g_running = 0;
    }
}

/* ================================================================
 * TCP 服务器
 * ================================================================ */

typedef struct {
    char     address[64];
    uint16_t port;
    int      max_connections;
} server_config_t;

/* 设置 socket 为非阻塞模式 */
static int socket_set_nonblock(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* 创建监听 socket */
static socket_t server_listen(server_config_t *cfg) {
    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCKET_INVALID) {
        LOG_ERROR("socket() failed: %d", (int)SOCKET_ERRNO);
        return SOCKET_INVALID;
    }

    /* 端口复用 */
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    if (strcmp(cfg->address, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        addr.sin_addr.s_addr = inet_addr(cfg->address);
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind(%s:%d) failed: %d", cfg->address, cfg->port,
                  (int)SOCKET_ERRNO);
        CLOSE_SOCKET(listen_fd);
        return SOCKET_INVALID;
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        LOG_ERROR("listen() failed: %d", (int)SOCKET_ERRNO);
        CLOSE_SOCKET(listen_fd);
        return SOCKET_INVALID;
    }

    LOG_INFO("listening on %s:%d (max %d connections)",
             cfg->address, cfg->port, cfg->max_connections);
    return listen_fd;
}

/* 处理已建立的连接上的数据 */
static void handle_session(session_t *s) {
    uint8_t tmp_buf[65536];
    decoded_msg_t msg;

    while (1) {
#ifdef _WIN32
        int n = recv(s->fd, (char *)tmp_buf, sizeof(tmp_buf), 0);
#else
        ssize_t n = recv(s->fd, tmp_buf, sizeof(tmp_buf), 0);
#endif
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
#endif
            LOG_ERROR("recv(session %d) error: %d", s->fd, (int)SOCKET_ERRNO);
            session_remove(s->fd);
            return;
        }
        if (n == 0) {
            /* 对端关闭连接 */
            LOG_INFO("session %d closed by peer", s->fd);
            session_remove(s->fd);
            return;
        }

        /* 喂入解码器, 可能一次收到多帧 */
        size_t consumed = 0;
        const uint8_t *ptr = tmp_buf;
        while (consumed < (size_t)n) {
            int rc = protocol_decode(s, ptr, (size_t)n - consumed, &msg);
            if (rc < 0) {
                LOG_ERROR("session %d protocol error", s->fd);
                session_remove(s->fd);
                return;
            }
            if (rc == 0) break;  /* 需要更多数据 */

            /* 成功解码一条消息 — 更新活跃时间 */
            session_touch(s);

            /* 处理解码结果 */
            switch (msg.type) {
            case DECODE_DATA_BLOCK:
                /* 写入目标磁盘 */
                if (block_writer_write(msg.devno, msg.offset,
                                       msg.data, msg.data_len) == 0) {
                    ack_send_block(s->fd, msg.devno, msg.data_len, msg.offset);
                }
                break;

            case DECODE_CTL_INCREMENTAL:
                s->incremental = 1;
                LOG_INFO("session %d incremental mode enabled", s->fd);
                break;

            case DECODE_CTL_END_INCREMENTAL:
                LOG_INFO("session %d incremental round complete, fsyncing...", s->fd);
                block_writer_fsync_all();
                ack_send_done(s->fd);
                LOG_INFO("session %d incremental round finished", s->fd);
                break;

            default:
                LOG_WARN("session %d unknown decode result %d", s->fd, msg.type);
                break;
            }

            /* 将 ptr 前进到下一个帧 */
            consumed = (size_t)n - s->recv_len;
            ptr = tmp_buf + consumed;
            /* (session.recv_len 已在 protocol_decode 中更新为剩余数据量) */
        }
    }
}

/* 服务端主循环 (简易 select 轮询, 不引入 libuv 依赖) */
static int server_run(socket_t listen_fd, server_config_t *cfg) {
    fd_set readfds;
    struct timeval tv;

    while (g_running) {
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        socket_t max_fd = listen_fd;

        /* 收集当前所有会话 fd */
        session_foreach([](session_t *s, void *arg) -> int {
            fd_set *fds = (fd_set *)arg;
            FD_SET(s->fd, fds);
            return 0;
        }, &readfds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int nfds = select((int)max_fd + 1, &readfds, NULL, NULL, &tv);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select() error: %s", strerror(errno));
            break;
        }

        /* 新连接 */
        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            socket_t client_fd = accept(listen_fd,
                                        (struct sockaddr *)&client_addr,
                                        &addr_len);
            if (client_fd != SOCKET_INVALID) {
                if (session_count() >= cfg->max_connections) {
                    LOG_WARN("max connections reached, rejecting %s",
                             inet_ntoa(client_addr.sin_addr));
                    CLOSE_SOCKET(client_fd);
                } else {
                    socket_set_nonblock(client_fd);
                    session_t *s = session_create(client_fd,
                                                  inet_ntoa(client_addr.sin_addr));
                    if (!s) {
                        CLOSE_SOCKET(client_fd);
                    }
                }
            }
        }

        /* 已有会话的数据到达 */
        session_foreach([](session_t *s, void *arg) -> int {
            fd_set *fds = (fd_set *)arg;
            if (FD_ISSET(s->fd, fds)) {
                handle_session(s);
            }
            return 0;
        }, &readfds);
    }

    return 0;
}

/* ================================================================
 * 入口
 * ================================================================ */

int main(int argc, char *argv[]) {
    const char *config_path = "receiver.json";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("go2cloud receiver — block-level migration server\n");
            printf("Usage: %s --config <path>\n", argv[0]);
            printf("Default config: receiver.json\n");
            return 0;
        }
    }

    /* 读取配置文件 */
    char *config_json = read_file_all(config_path);
    if (!config_json) {
        fprintf(stderr, "Cannot read config file: %s\n", config_path);
        fprintf(stderr, "Using defaults: listen 0.0.0.0:3389, target /tmp/receiver_disk0.img\n");
    }

    /* 解析日志配置 */
    int log_level = LOG_INFO;
    char log_path[256] = {0};
    if (config_json) {
        const char *log_section = json_nav(config_json, "Log", NULL);
        if (log_section) {
            log_level = json_read_int(log_section, "Level", LOG_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }
    }
    log_init(log_level, log_path[0] ? log_path : NULL);

    LOG_INFO("go2cloud receiver starting...");

    /* 解析监听配置 */
    server_config_t srv_cfg = {
        .address        = "0.0.0.0",
        .port           = 3389,
        .max_connections = 7
    };
    if (config_json) {
        const char *listen_section = json_nav(config_json, "Listen", NULL);
        if (listen_section) {
            char addr[64];
            if (json_read_str(listen_section, "Address", addr, sizeof(addr))) {
                strncpy(srv_cfg.address, addr, sizeof(srv_cfg.address) - 1);
            }
            srv_cfg.port = (uint16_t)json_read_int(listen_section, "Port", 3389);
            srv_cfg.max_connections = json_read_int(listen_section, "MaxConnections", 7);
        }
    }

    /* 初始化模块 */
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    session_mgr_init();
    block_writer_init();

    /* 解析目标磁盘配置 */
    if (config_json) {
        const char *target_section = json_nav(config_json, "Target", NULL);
        if (target_section) {
            const char *disks_section = json_nav(target_section, "Disks", NULL);
            if (disks_section) {
                /* 查找所有 "数字" 键: "0", "1", ... */
                for (int i = 0; i < MAX_TARGET_DISKS; i++) {
                    char key[16];
                    snprintf(key, sizeof(key), "\"%d\"", i);
                    const char *p = strstr(disks_section, key);
                    if (!p) break;
                    p = strchr(p + strlen(key), ':');
                    if (!p) continue;
                    while (*p && (*p == ':' || *p == ' ' || *p == '\t' || *p == '\n')) p++;
                    if (*p != '"') continue;
                    p++;
                    const char *end = strchr(p, '"');
                    if (!end) continue;
                    size_t n = MIN((size_t)(end - p), (size_t)511);
                    char path[512];
                    memcpy(path, p, n);
                    path[n] = '\0';
                    block_writer_register(i, path);
                }
            }
        }
    }

    /* 如果没有注册任何磁盘, 使用默认值 */
    if (block_writer_total_blocks() == 0 && block_writer_total_bytes() == 0) {
        /* 这是一个启发式检查 — 如果未注册则添加一个默认盘 */
        block_writer_register(0, "/tmp/receiver_disk0.img");
    }

    /* 打开目标磁盘 */
    if (block_writer_open_all() < 0) {
        LOG_ERROR("Failed to open target disks");
        free(config_json);
        return 1;
    }

    LOG_INFO("Target disks opened successfully");

    /* 创建监听 socket */
    socket_t listen_fd = server_listen(&srv_cfg);
    if (listen_fd == SOCKET_INVALID) {
        LOG_ERROR("Failed to create listen socket");
        free(config_json);
        return 1;
    }

    /* 注册信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 主循环 */
    LOG_INFO("Receiver ready, waiting for connections...");
    server_run(listen_fd, &srv_cfg);

    /* 清理 */
    LOG_INFO("Shutting down...");
    CLOSE_SOCKET(listen_fd);
    block_writer_fsync_all();
    block_writer_close_all();
    log_close();
#ifdef _WIN32
    WSACleanup();
#endif
    free(config_json);
    LOG_INFO("Receiver stopped.");
    return 0;
}
