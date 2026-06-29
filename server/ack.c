/*
 * ack.c — ACK 响应生成模块实现
 *
 * 所有 ACK 使用 server_response_t 结构体 (20 字节) 直接发送。
 * 无编码/分帧 — 对端直接 recv(20) 读取。
 */

#include "ack.h"
#include "../include/protocol.h"
#include "log.h"

#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <errno.h>
#endif

/* 发送 20 字节响应 (处理短写) */
static int send_response(int fd, server_response_t *resp) {
    const char *data = (const char *)resp;
    size_t total = sizeof(server_response_t);
    size_t sent = 0;

    while (sent < total) {
#ifdef _WIN32
        int n = send(fd, data + sent, (int)(total - sent), 0);
#else
        ssize_t n = send(fd, data + sent, total - sent, MSG_NOSIGNAL);
#endif
        if (n < 0) {
#ifdef _WIN32
            LOG_ERROR("send response failed: fd=%d, err=%d", fd, WSAGetLastError());
#else
            if (errno == EINTR) continue;
            LOG_ERROR("send response failed: fd=%d, err=%s", fd, strerror(errno));
#endif
            return -1;
        }
        sent += n;
    }
    return 0;
}

int ack_send_block(int fd, int32_t devno, int32_t size, int64_t offset) {
    server_response_t resp = {
        .type   = RESPONSE_ACK,
        .devno  = devno,
        .size   = size,
        .offset = offset
    };
    LOG_DEBUG("ACK block: devno=%d offset=%lld size=%d",
              devno, (long long)offset, size);
    return send_response(fd, &resp);
}

int ack_send_done(int fd) {
    server_response_t resp = {
        .type   = RESPONSE_SERVER_DONE,
        .devno  = 0,
        .size   = 0,
        .offset = 0
    };
    LOG_INFO("send server done (type=2)");
    return send_response(fd, &resp);
}

int ack_send_binlog(int fd, int32_t devno, int32_t size, int64_t offset) {
    server_response_t resp = {
        .type   = RESPONSE_BINLOG,
        .devno  = devno,
        .size   = size,
        .offset = offset
    };
    LOG_WARN("send binlog alert: devno=%d offset=%lld size=%d",
             devno, (long long)offset, size);
    return send_response(fd, &resp);
}
