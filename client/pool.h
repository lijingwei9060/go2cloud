/*
 * pool.h — Socket 连接池模块
 *
 * 维护与接收端 (服务端) 的多个并发 TCP 连接。
 * 每个连接是非阻塞的, 连接池负责:
 *   - 建立连接 (带重试)
 *   - 分配/归还可用连接
 *   - 检测并回收过期连接
 *   - 接收 ACK 响应 (20 字节固定结构)
 *
 * 目标连接数: SOCKET_POOL_TARGET (7)
 */

#ifndef CLIENT_POOL_H
#define CLIENT_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET socket_t;
#define SOCKET_ERR  INVALID_SOCKET
#else
typedef int socket_t;
#define SOCKET_ERR  (-1)
#endif

#include "../include/protocol.h"

/* 单个池连接 */
typedef struct {
    socket_t  fd;                         /* TCP socket */
    char      server_ip[64];              /* 服务端 IP */
    uint16_t  server_port;                /* 服务端端口 */
    int       in_use;                     /* 1 = 正在使用, 0 = 空闲 */
    time_t    last_active;                /* 最后活动时间 */
    time_t    created_at;                 /* 创建时间 */
    uint64_t  blocks_sent;                /* 此连接上已发送的块数 */
    uint64_t  bytes_sent;                 /* 此连接上已发送的字节数 */

    /* 发送临时缓冲区 (Zstd 压缩 + 帧化), 堆分配避免栈溢出 */
    uint8_t  *send_scratch;
    size_t    send_scratch_cap;
} pool_conn_t;

/* 连接池 */
typedef struct {
    pool_conn_t conns[SOCKET_POOL_TARGET];
    int         count;                    /* 当前已建立连接数 */
    void       *mutex;                    /* 平台互斥锁 */
} socket_pool_t;

/* 初始化连接池 */
int pool_init(socket_pool_t *p, const char *server_ip, uint16_t server_port);

/* 销毁连接池 (关闭所有连接) */
void pool_destroy(socket_pool_t *p);

/*
 * 获取一个空闲连接。
 * 如果有空闲连接则返回其指针, 并标记 in_use=1。
 * 如果可以建立更多连接 (count < SOCKET_POOL_TARGET), 则创建新连接。
 * 如果所有连接都在使用中且已达到上限, 返回 NULL。
 */
pool_conn_t *pool_acquire(socket_pool_t *p);

/* 归还一个连接到空闲状态 */
void pool_release(socket_pool_t *p, pool_conn_t *c);

/*
 * 扫描并关闭过期连接 (超过 STALE_SOCKET_TIMEOUT_SEC 无活动)。
 * 返回关闭的连接数。
 */
int pool_reap_stale(socket_pool_t *p);

/*
 * 在一条连接上尝试接收一个 20 字节的 ACK 响应。
 * 非阻塞: 如果没有数据则返回 0。
 *
 * 返回:
 *   -1  — 错误或连接关闭
 *    0  — 无数据可用 (EAGAIN)
 *    1  — 成功接收一个 server_response_t
 */
int pool_recv_ack(pool_conn_t *c, server_response_t *resp);

/* 获取当前连接数 */
int pool_count(socket_pool_t *p);

/*
 * 更新连接活动时间。
 * 在每次成功发送或接收后调用。
 */
void pool_touch(pool_conn_t *c);

#endif /* CLIENT_POOL_H */
