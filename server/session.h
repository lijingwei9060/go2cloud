/*
 * session.h — 客户端会话管理
 *
 * 管理每个连接的客户端会话, 跟踪连接状态和传输统计。
 * 服务端为每个 TCP 连接创建一个 session。
 */

#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <stdint.h>
#include <time.h>

#define MAX_SESSIONS       16     /* 最大并发会话数 */
#define REMOTE_ID_MAX_LEN  256   /* 主机名最大长度 */

/* 客户端会话 */
typedef struct session {
    int      fd;                  /* TCP socket 文件描述符 */
    char     remote_addr[64];     /* 对端 IP 地址 (日志用) */
    char     remote_id[REMOTE_ID_MAX_LEN]; /* 客户端主机名 */
    int      incremental;         /* 是否增量模式 */
    time_t   connected_at;        /* 连接建立时间 */
    time_t   last_active;         /* 最后活动时间 */
    uint64_t blocks_received;     /* 已接收块数 */
    uint64_t bytes_received;      /* 已接收字节数 */

    /* TCP 分帧层接收缓冲区 */
    uint8_t  recv_buf[4 * 1024 * 1024]; /* 4MB 接收缓冲 */
    size_t   recv_len;            /* 缓冲区中已积累的字节数 */
    int      expecting_frame;     /* 是否正在等待一个完整帧 */
    uint32_t frame_len;           /* 当前帧的预期长度 (来自 4B 头) */
} session_t;

/* 初始化会话管理器 */
void session_mgr_init(void);

/* 创建新会话 */
session_t *session_create(int fd, const char *remote_addr);

/* 查找会话 (按 fd) */
session_t *session_find(int fd);

/* 移除并销毁会话 */
void session_remove(int fd);

/* 获取活跃会话数 */
int session_count(void);

/* 遍历所有会话 (回调返回非零停止) */
void session_foreach(int (*cb)(session_t *, void *), void *arg);

/* 更新最后活动时间 */
void session_touch(session_t *s);

#endif /* SERVER_SESSION_H */
