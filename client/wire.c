/*
 * wire.c — TCP 有线协议模块实现
 *
 * 发送路径: MsgPack → Zstd 压缩 → "abc" 前缀 → 4B BE 长度 → send()
 * 控制消息: 原始字符串 → 4B BE 长度 → send()
 */

#include "wire.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

/* 将 32 位值从主机序转为大端 */
static inline uint32_t host_to_be32(uint32_t val) {
#ifdef _WIN32
    return htonl(val);
#else
    return __builtin_bswap32(val);
#endif
}

/*
 * 通过非阻塞 socket 发送完整数据, 处理短写。
 * 循环调用 send() 直到所有数据发出或遇到 EWOULDBLOCK。
 */
static int send_all(socket_t fd, const uint8_t *data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        int n = send(fd, (const char *)(data + sent), (int)(len - sent), 0);
#else
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
            LOG_ERROR("wire send_all failed: %d", (int)n);
            return -1;
        }
        if (n == 0) {
            LOG_ERROR("wire send_all: connection closed by peer");
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

int wire_send(socket_t fd,
              const uint8_t *msg_data, size_t msg_len,
              int zstd_level,
              uint8_t *scratch_buf, size_t scratch_cap) {

    /* 第 3 层: Zstd 压缩 MsgPack 载荷 */
    size_t zstd_bound = ZSTD_COMPRESS_BOUND(msg_len);
    if (zstd_bound + FRAME_MAGIC_LEN + TCP_FRAME_HEADER_SIZE > scratch_cap) {
        LOG_ERROR("wire_send: scratch buffer too small (need %zu, have %zu)",
                  zstd_bound + FRAME_MAGIC_LEN + TCP_FRAME_HEADER_SIZE, scratch_cap);
        return -1;
    }

    /* 在 scratch 末尾留出空间放压缩数据, 前面留给帧头 */
    uint8_t *compressed = scratch_buf + TCP_FRAME_HEADER_SIZE + FRAME_MAGIC_LEN;
    size_t compressed_len = ZSTD_compress(compressed, zstd_bound,
                                          msg_data, msg_len, zstd_level);
    if (ZSTD_isError(compressed_len)) {
        LOG_ERROR("wire_send: ZSTD_compress failed: %s",
                  ZSTD_getErrorName(compressed_len));
        return -1;
    }

    size_t payload_len = FRAME_MAGIC_LEN + compressed_len;

    /* 第 2 层: 写入 "abc" 魔数 */
    scratch_buf[TCP_FRAME_HEADER_SIZE + 0] = FRAME_MAGIC_B0;  /* 'a' */
    scratch_buf[TCP_FRAME_HEADER_SIZE + 1] = FRAME_MAGIC_B1;  /* 'b' */
    scratch_buf[TCP_FRAME_HEADER_SIZE + 2] = FRAME_MAGIC_B2;  /* 'c' */

    /* 第 1 层: 写入 4 字节大端帧长度 */
    uint32_t be_len = host_to_be32((uint32_t)payload_len);
    memcpy(scratch_buf, &be_len, TCP_FRAME_HEADER_SIZE);

    size_t total_len = TCP_FRAME_HEADER_SIZE + payload_len;

    LOG_TRACE("wire_send: msg=%zu zstd=%zu → frame=%zu bytes (level %d)",
              msg_len, compressed_len, total_len, zstd_level);

    return send_all(fd, scratch_buf, total_len);
}

int wire_send_control(socket_t fd, const char *str, size_t len) {
    /* 控制消息: 4B BE 长度 + 原始字符串 */
    uint8_t header[TCP_FRAME_HEADER_SIZE];
    uint32_t be_len = host_to_be32((uint32_t)len);
    memcpy(header, &be_len, TCP_FRAME_HEADER_SIZE);

    if (send_all(fd, header, TCP_FRAME_HEADER_SIZE) != 0) return -1;
    if (send_all(fd, (const uint8_t *)str, len) != 0) return -1;

    LOG_TRACE("wire_send_control: sent %zu bytes", len);
    return 0;
}
