/*
 * main.c — server (Receiver) entry point
 *
 * go2cloud block-level migration server. Responsibilities:
 *   1. TCP listen for client connections
 *   2. Decode wire protocol (TCP frame -> "abc" -> Zstd -> MsgPack)
 *   3. Write block data to target disks
 *   4. Return ACK responses
 *   5. Handle incremental protocol (ctlIncremental / ctlEndIncremental)
 *
 * Usage:
 *   receiver --config receiver.json
 *
 * Config file format (receiver.json):
 *   {
 *     "Listen": {"Address": "0.0.0.0", "Port": 3389, "MaxConnections": 7},
 *     "Target": {"Disks": {"0": "/dev/sdb", "1": "/dev/sdc"}},
 *     "Log": {"Level": "info", "Path": "/var/log/receiver.log"}
 *   }
 *
 * Build (Linux):
 *   gcc -O2 -Wall -o receiver server/*.c -lzstd -lpthread
 *
 * Build (Windows — MSVC):
 *   cl /O2 /utf-8 /Fe:receiver.exe server\*.c /Iinclude ^
 *      /I<vcpkg>\installed\x64-windows\include ^
 *      /link libzstd.lib ws2_32.lib
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
 * Simple JSON parser (reads only the fields needed for config,
 * avoiding third-party library dependencies)
 * ================================================================ */

#define JSON_MAX_DEPTH   16
#define JSON_MAX_KEY     64
#define JSON_MAX_STR     512

/* Read a top-level integer field from a JSON string */
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

/* Read a top-level string field from a JSON string */
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

/* Navigate to a nested object: "Parent"."Child" */
static const char *json_nav(const char *json, const char *parent, const char *child) {
    char search[JSON_MAX_KEY + 8];
    snprintf(search, sizeof(search), "\"%s\"", parent);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), '{');
    if (!p) return NULL;
    (void)child;
    return p;
}

/* Read entire file contents */
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
 * Signal handling
 * ================================================================ */

static volatile int g_running = 1;

static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        LOG_INFO("received signal %d, shutting down...", sig);
        g_running = 0;
    }
}

/* ================================================================
 * TCP server
 * ================================================================ */

typedef struct {
    char     address[64];
    uint16_t port;
    int      max_connections;
} server_config_t;

/* Set socket to non-blocking mode */
static int socket_set_nonblock(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* Create listening socket */
static socket_t server_listen(server_config_t *cfg) {
    socket_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == SOCKET_INVALID) {
        LOG_ERROR("socket() failed: %d", (int)SOCKET_ERRNO);
        return SOCKET_INVALID;
    }

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

/* Handle data on an established connection */
static void handle_session(session_t *s) {
    uint8_t tmp_buf[65536];

    /* Heap-allocated: struct contains 1MB+ data buffer, too large for stack */
    decoded_msg_t *msg = calloc(1, sizeof(decoded_msg_t));
    if (!msg) {
        LOG_ERROR("session %d out of memory for decode buffer", s->fd);
        session_remove(s->fd);
        return;
    }

    while (1) {
#ifdef _WIN32
        int n = recv(s->fd, (char *)tmp_buf, sizeof(tmp_buf), 0);
#else
        ssize_t n = recv(s->fd, tmp_buf, sizeof(tmp_buf), 0);
#endif
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) { free(msg); return; }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) { free(msg); return; }
#endif
            LOG_ERROR("recv(session %d) error: %d", s->fd, (int)SOCKET_ERRNO);
            free(msg);
            session_remove(s->fd);
            return;
        }
        if (n == 0) {
            LOG_INFO("session %d closed by peer", s->fd);
            free(msg);
            session_remove(s->fd);
            return;
        }

        /* Feed freshly received data to decoder once.
         * protocol_decode appends to session->recv_buf, and any leftover
         * partial frame stays buffered.  We then loop with zero-length
         * calls to flush any complete frames formed by concatenation. */
        {
            int rc = protocol_decode(s, tmp_buf, (size_t)n, msg);
            if (rc < 0) {
                LOG_ERROR("session %d protocol error", s->fd);
                free(msg);
                session_remove(s->fd);
                return;
            }

            /* Keep decoding while there are complete frames buffered */
            while (rc == 1) {
                session_touch(s);

                switch (msg->type) {
                case DECODE_DATA_BLOCK:
                    if (block_writer_write(msg->devno, msg->offset,
                                           msg->data, msg->data_len) == 0) {
                        ack_send_block(s->fd, msg->devno, msg->data_len, msg->offset);

                        /* BINLOG monitoring: alert client when pending write buffer
                         * exceeds BINLOG_SIZE_LIMIT (10 GB), so client can throttle
                         * or trigger an early incremental round. */
                        if (block_writer_pending_bytes() >= BINLOG_SIZE_LIMIT) {
                            ack_send_binlog(s->fd, msg->devno, msg->data_len, msg->offset);
                        }
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
                    LOG_WARN("session %d unknown decode result %d", s->fd, msg->type);
                    break;
                }

                /* Try to decode next frame already in recv_buf (no new data) */
                rc = protocol_decode(s, NULL, 0, msg);
            }
            if (rc < 0) {
                LOG_ERROR("session %d protocol error", s->fd);
                free(msg);
                session_remove(s->fd);
                return;
            }
        }
    }
}

/* -- session_foreach callback: find maximum fd -- */
static int find_max_fd_cb(session_t *s, void *arg) {
    socket_t *max_fd = (socket_t *)arg;
    if (s->fd > *max_fd) *max_fd = s->fd;
    return 0;
}

/* -- session_foreach callback: collect fd into fd_set -- */
static int collect_fds_cb(session_t *s, void *arg) {
    fd_set *fds = (fd_set *)arg;
    FD_SET(s->fd, fds);
    return 0;
}

/* -- session_foreach callback: check and handle active fds -- */
static int handle_active_cb(session_t *s, void *arg) {
    fd_set *fds = (fd_set *)arg;
    if (FD_ISSET(s->fd, fds)) {
        handle_session(s);
    }
    return 0;
}

/* Server main loop (simple select-based polling) */
static int server_run(socket_t listen_fd, server_config_t *cfg) {
    fd_set readfds;
    struct timeval tv;

    while (g_running) {
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        socket_t max_fd = listen_fd;

        /* Collect all session fds */
        session_foreach(collect_fds_cb, &readfds);

        /* Find max fd for select() — walk all sessions to track highest fd */
        session_foreach(find_max_fd_cb, &max_fd);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int nfds = select((int)max_fd + 1, &readfds, NULL, NULL, &tv);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select() error: %s", strerror(errno));
            break;
        }

        /* New connection */
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

        /* Existing session data arrival */
        session_foreach(handle_active_cb, &readfds);
    }

    return 0;
}

/* ================================================================
 * Entry point
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

    /* Read config file */
    char *config_json = read_file_all(config_path);
    if (!config_json) {
        printf("Cannot read config file: %s\n", config_path);
        printf("Using defaults: listen 0.0.0.0:3389, target /tmp/receiver_disk0.img\n");
    }

    /* Parse log config */
    int log_level = LOG_LEVEL_INFO;
    char log_path[256] = {0};
    if (config_json) {
        const char *log_section = json_nav(config_json, "Log", NULL);
        if (log_section) {
            log_level = json_read_int(log_section, "Level", LOG_LEVEL_INFO);
            json_read_str(log_section, "Path", log_path, sizeof(log_path));
        }
    }
    log_init(log_level, log_path[0] ? log_path : NULL);

    if (log_path[0]) {
        printf("Log file: %s\n", log_path);
    }
    printf("go2cloud receiver starting...\n");
    LOG_INFO("go2cloud receiver starting...");

    /* Parse listen config */
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

    /* Init modules */
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
    session_mgr_init();
    block_writer_init();

    /* Parse target disk config */
    if (config_json) {
        const char *target_section = json_nav(config_json, "Target", NULL);
        if (target_section) {
            const char *disks_section = json_nav(target_section, "Disks", NULL);
            if (disks_section) {
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

    /* If no disks registered, use default */
    if (!block_writer_has_registered()) {
        printf("No target disks configured, using default: /tmp/receiver_disk0.img\n");
        block_writer_register(0, "/tmp/receiver_disk0.img");
    }

    /* Open target disks */
    if (block_writer_open_all() < 0) {
        fprintf(stderr, "ERROR: Failed to open target disks\n");
        LOG_ERROR("Failed to open target disks");
        free(config_json);
        return 1;
    }

    printf("Target disks opened successfully\n");
    LOG_INFO("Target disks opened successfully");

    /* Create listen socket */
    socket_t listen_fd = server_listen(&srv_cfg);
    if (listen_fd == SOCKET_INVALID) {
        LOG_ERROR("Failed to create listen socket");
        free(config_json);
        return 1;
    }

    /* Register signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Main loop */
    printf("Listening on %s:%d (max %d connections)\n",
           srv_cfg.address, srv_cfg.port, srv_cfg.max_connections);
    LOG_INFO("Receiver ready, waiting for connections...");
    server_run(listen_fd, &srv_cfg);

    /* Cleanup */
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
