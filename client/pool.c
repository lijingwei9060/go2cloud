/*
 * pool.c — Socket 连接池模块实现
 *
 * 非阻塞 TCP 连接池, 支持:
 *   - 并发连接建立 (最多 SOCKET_POOL_TARGET 个)
 *   - 空闲连接获取/归还
 *   - 过期连接回收
 *   - 非阻塞 ACK 接收
 */

#include "pool.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

typedef CRITICAL_SECTION mutex_t;
#define MUTEX_INIT(m) InitializeCriticalSection(m)
#define MUTEX_LOCK(m) EnterCriticalSection(m)
#define MUTEX_UNLOCK(m) LeaveCriticalSection(m)
#define MUTEX_DEL(m)  DeleteCriticalSection(m)

static int socket_set_nonblock(socket_t fd) {
    u_long mode = 1;
    return ioctlsocket(fd, FIONBIO, &mode);
}
#define SOCK_LAST_ERR  WSAGetLastError()
#define SOCK_WOULDBLOCK WSAEWOULDBLOCK
#define CLOSE_SOCK(s)  closesocket(s)

#else
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

typedef pthread_mutex_t mutex_t;
#define MUTEX_INIT(m) pthread_mutex_init(m, NULL)
#define MUTEX_LOCK(m) pthread_mutex_lock(m)
#define MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
#define MUTEX_DEL(m)  pthread_mutex_destroy(m)

static int socket_set_nonblock(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
#define SOCK_LAST_ERR  errno
#define SOCK_WOULDBLOCK EAGAIN
#define CLOSE_SOCK(s)  close(s)
#endif

/* 建立一条到服务端的 TCP 连接 */
static socket_t pool_connect(const char *ip, uint16_t port) {
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == SOCKET_ERR) {
        LOG_ERROR("pool_connect: socket() failed: %d", (int)SOCK_LAST_ERR);
        return SOCKET_ERR;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
#ifdef _WIN32
        if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
        if (errno != EINPROGRESS)
#endif
        {
            LOG_ERROR("pool_connect: connect(%s:%d) failed", ip, (int)port);
            CLOSE_SOCK(fd);
            return SOCKET_ERR;
        }
        /* 非阻塞连接进行中 — 在后续 send/recv 中完成 */
    }

    socket_set_nonblock(fd);
    return fd;
}

int pool_init(socket_pool_t *p, const char *server_ip, uint16_t server_port) {
    memset(p, 0, sizeof(*p));
    p->mutex = malloc(sizeof(mutex_t));
    if (!p->mutex) return -1;
    MUTEX_INIT((mutex_t *)p->mutex);

    /* 预建立连接 */
    for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
        socket_t fd = pool_connect(server_ip, server_port);
        if (fd == SOCKET_ERR) {
            if (i == 0) {
                LOG_ERROR("pool_init: cannot establish any connection to %s:%d",
                          server_ip, (int)server_port);
                MUTEX_DEL((mutex_t *)p->mutex);
                free(p->mutex);
                return -1;
            }
            LOG_WARN("pool_init: only %d/%d connections established",
                     i, SOCKET_POOL_TARGET);
            break;
        }

        pool_conn_t *c = &p->conns[i];
        memset(c, 0, sizeof(*c));
        c->fd             = fd;
        c->server_port    = server_port;
        c->in_use         = 0;
        c->last_active    = time(NULL);
        c->created_at     = time(NULL);
        c->send_scratch = malloc(SEND_SCRATCH_SIZE);
        if (!c->send_scratch) {
            CLOSE_SOCK(fd);
            break;
        }
        c->send_scratch_cap = SEND_SCRATCH_SIZE;
        strncpy(c->server_ip, server_ip, sizeof(c->server_ip) - 1);

        p->count++;
        LOG_INFO("pool: connected #%d to %s:%d (fd=%d)",
                 i, server_ip, (int)server_port, (int)fd);
    }

    return 0;
}

void pool_destroy(socket_pool_t *p) {
    MUTEX_LOCK((mutex_t *)p->mutex);
    for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
        if (p->conns[i].fd != SOCKET_ERR) {
            CLOSE_SOCK(p->conns[i].fd);
            p->conns[i].fd = SOCKET_ERR;
        }
        if (p->conns[i].send_scratch) {
            free(p->conns[i].send_scratch);
            p->conns[i].send_scratch = NULL;
        }
    }
    p->count = 0;
    MUTEX_UNLOCK((mutex_t *)p->mutex);

    if (p->mutex) {
        MUTEX_DEL((mutex_t *)p->mutex);
        free(p->mutex);
        p->mutex = NULL;
    }
}

pool_conn_t *pool_acquire(socket_pool_t *p) {
    MUTEX_LOCK((mutex_t *)p->mutex);

    /* 先查找空闲连接 */
    for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
        if (p->conns[i].fd != SOCKET_ERR && !p->conns[i].in_use) {
            p->conns[i].in_use = 1;
            MUTEX_UNLOCK((mutex_t *)p->mutex);
            return &p->conns[i];
        }
    }

    /* 尝试创建新连接 */
    if (p->count < SOCKET_POOL_TARGET) {
        for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
            if (p->conns[i].fd == SOCKET_ERR) {
                socket_t fd = pool_connect(p->conns[i].server_ip,
                                           p->conns[i].server_port);
                if (fd != SOCKET_ERR) {
                    pool_conn_t *c = &p->conns[i];
                    if (!c->send_scratch) {
                        c->send_scratch = malloc(SEND_SCRATCH_SIZE);
                        if (!c->send_scratch) {
                            CLOSE_SOCK(fd);
                            MUTEX_UNLOCK((mutex_t *)p->mutex);
                            return NULL;
                        }
                        c->send_scratch_cap = SEND_SCRATCH_SIZE;
                    }
                    c->fd          = fd;
                    c->in_use      = 1;
                    c->last_active = time(NULL);
                    p->count++;
                    MUTEX_UNLOCK((mutex_t *)p->mutex);
                    return c;
                }
                break;
            }
        }
    }

    MUTEX_UNLOCK((mutex_t *)p->mutex);
    return NULL;  /* 无可用连接 */
}

void pool_release(socket_pool_t *p, pool_conn_t *c) {
    MUTEX_LOCK((mutex_t *)p->mutex);
    c->in_use = 0;
    MUTEX_UNLOCK((mutex_t *)p->mutex);
}

void pool_touch(pool_conn_t *c) {
    c->last_active = time(NULL);
}

int pool_reap_stale(socket_pool_t *p) {
    int closed = 0;
    time_t now = time(NULL);

    MUTEX_LOCK((mutex_t *)p->mutex);
    for (int i = 0; i < SOCKET_POOL_TARGET; i++) {
        pool_conn_t *c = &p->conns[i];
        if (c->fd != SOCKET_ERR && !c->in_use) {
            if (now - c->last_active > STALE_SOCKET_TIMEOUT_SEC) {
                LOG_INFO("pool: closing stale connection #%d (fd=%d, idle=%lds)",
                         i, (int)c->fd, (long)(now - c->last_active));
                CLOSE_SOCK(c->fd);
                c->fd = SOCKET_ERR;
                p->count--;
                closed++;
            }
        }
    }
    MUTEX_UNLOCK((mutex_t *)p->mutex);
    return closed;
}

int pool_recv_ack(pool_conn_t *c, server_response_t *resp) {
    uint8_t buf[SERVER_RESPONSE_SIZE];
    size_t received = 0;

    while (received < SERVER_RESPONSE_SIZE) {
#ifdef _WIN32
        int n = recv(c->fd, (char *)(buf + received),
                     (int)(SERVER_RESPONSE_SIZE - received), 0);
#else
        ssize_t n = recv(c->fd, buf + received,
                         SERVER_RESPONSE_SIZE - received, MSG_DONTWAIT);
#endif
        if (n < 0) {
            if (SOCK_LAST_ERR == SOCK_WOULDBLOCK) {
                return 0;  /* 无数据 */
            }
            LOG_ERROR("pool_recv_ack: recv error on fd=%d: %d",
                      (int)c->fd, (int)SOCK_LAST_ERR);
            return -1;
        }
        if (n == 0) return -1;  /* 连接关闭 */
        received += (size_t)n;
    }

    /* 直接 memcpy — 服务端以本地字节序发送 server_response_t 原始字节 */
    memcpy(resp, buf, SERVER_RESPONSE_SIZE);
    return 1;
}

int pool_count(socket_pool_t *p) {
    return p->count;
}
