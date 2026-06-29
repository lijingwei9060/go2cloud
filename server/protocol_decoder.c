/*
 * protocol_decoder.c — 协议解码模块实现
 *
 * 状态机驱动解码: 累积 TCP 字节 → 解析 4B 帧头 → 缓冲完整帧 → 解码
 *
 * TCP 粘包/拆包处理:
 *   - 拆包: recv_buf 可能未收完一个完整帧 → 返回 PENDING, 等待更多数据
 *   - 粘包: recv_buf 可能包含多个帧 → 处理完一帧后继续处理剩余数据
 */

#include "session.h"
#include "protocol_decoder.h"
#include "log.h"
#include "../include/protocol.h"

#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <winsock2.h>  /* htonl, ntohl */
#else
#include <arpa/inet.h>  /* htonl, ntohl */
#endif

/* Zstd 解压 — 使用 frame content size */
#include <zstd.h>

/* ================================================================
 * MsgPack 轻量解码器 (仅支持本协议所需类型)
 * ================================================================ */

/*
 * 读取 MsgPack 整数 (自选最短编码)。
 * 返回读取的字节数, 0 表示错误。
 */
static int msgpack_read_int(const uint8_t *buf, size_t buf_len,
                            int64_t *out_val) {
    if (buf_len < 1) return 0;
    uint8_t tag = buf[0];
    int used = 1;

    if (tag <= 0x7f) {
        /* fixint 正数: 0~127 */
        *out_val = tag;
        return 1;
    }
    if (tag >= 0xe0) {
        /* fixint 负数: -32 ~ -1 */
        *out_val = (int8_t)tag;
        return 1;
    }
    switch (tag) {
    case MSGPACK_INT8:   /* 0xd0 */
        if (buf_len < 2) return 0;
        *out_val = (int8_t)buf[1]; return 2;
    case MSGPACK_INT16:  /* 0xd1 */
        if (buf_len < 3) return 0;
        *out_val = (int16_t)((buf[1] << 8) | buf[2]); return 3;
    case MSGPACK_INT32:  /* 0xd2 */
        if (buf_len < 5) return 0;
        *out_val = (int32_t)((buf[1] << 24) | (buf[2] << 16) |
                             (buf[3] << 8)  | buf[4]);  return 5;
    case MSGPACK_INT64:  /* 0xd3 */
        if (buf_len < 9) return 0;
        *out_val = (int64_t)(((uint64_t)buf[1] << 56) |
                             ((uint64_t)buf[2] << 48) |
                             ((uint64_t)buf[3] << 40) |
                             ((uint64_t)buf[4] << 32) |
                             ((uint64_t)buf[5] << 24) |
                             ((uint64_t)buf[6] << 16) |
                             ((uint64_t)buf[7] << 8)  |
                             (uint64_t)buf[8]);
        return 9;
    default:
        return 0;  /* 不支持的类型 */
    }
}

/*
 * 解码 MsgPack fixmap(3){devno, offset, data}
 *
 * 有线格式:
 *   0x93                              — fixmap(3)
 *   0x00 <devno as int>               — key 0: devno
 *   0x01 <offset as int64>            — key 1: offset
 *   0x02 <data as bin8/16/32>         — key 2: data
 *
 * 返回: 0 = 成功, -1 = 格式错误
 */
static int msgpack_decode_block(const uint8_t *buf, size_t buf_len,
                                int32_t *out_devno, int64_t *out_offset,
                                const uint8_t **out_data, uint32_t *out_len) {
    size_t pos = 0;

    /* 验证 fixmap(3) */
    if (buf_len < 1 || buf[pos] != MSGPACK_FIXMAP_3) {
        LOG_ERROR("expected fixmap(3), got 0x%02x", buf_len > 0 ? buf[0] : 0);
        return -1;
    }
    pos++;

    /* 键 0: devno → 值: 整型 */
    if (pos >= buf_len || buf[pos] != 0x00) { LOG_ERROR("expected key 0"); return -1; }
    pos++;
    int64_t val64;
    int n = msgpack_read_int(buf + pos, buf_len - pos, &val64);
    if (n <= 0) { LOG_ERROR("failed to read devno"); return -1; }
    *out_devno = (int32_t)val64;
    pos += n;

    /* 键 1: offset → 值: 整型 (int64) */
    if (pos >= buf_len || buf[pos] != 0x01) { LOG_ERROR("expected key 1"); return -1; }
    pos++;
    n = msgpack_read_int(buf + pos, buf_len - pos, out_offset);
    if (n <= 0) { LOG_ERROR("failed to read offset"); return -1; }
    pos += n;

    /* 键 2: data → 值: bin8/16/32 */
    if (pos >= buf_len || buf[pos] != 0x02) { LOG_ERROR("expected key 2"); return -1; }
    pos++;
    if (pos >= buf_len) { LOG_ERROR("unexpected end for bin tag"); return -1; }
    uint8_t bin_tag = buf[pos];
    pos++;

    uint32_t bin_len = 0;
    if (bin_tag == MSGPACK_BIN8) {       /* 0xc4 */
        if (pos >= buf_len) return -1;
        bin_len = buf[pos]; pos++;
    } else if (bin_tag == MSGPACK_BIN16) { /* 0xc5 */
        if (pos + 1 >= buf_len) return -1;
        bin_len = ((uint32_t)buf[pos] << 8) | buf[pos + 1]; pos += 2;
    } else if (bin_tag == MSGPACK_BIN32) { /* 0xc6 */
        if (pos + 3 >= buf_len) return -1;
        bin_len = ((uint32_t)buf[pos] << 24) | ((uint32_t)buf[pos + 1] << 16) |
                  ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3]; pos += 4;
    } else {
        LOG_ERROR("expected bin tag, got 0x%02x", bin_tag);
        return -1;
    }

    if (pos + bin_len > buf_len) {
        LOG_ERROR("bin data truncated: need %u, have %zu", bin_len, buf_len - pos);
        return -1;
    }

    *out_data = buf + pos;
    *out_len  = bin_len;
    pos += bin_len;

    return 0;
}

/* ================================================================
 * 协议解码状态机
 * ================================================================ */

/*
 * 解码入口: 喂入新收到的原始 TCP 数据。
 *
 * 协议分帧状态机 (在 session_t 中维护):
 *   STATE 1 — 等待 4B 帧头: recv_buf 累积直到 ≥ 4 字节
 *   STATE 2 — 等待帧体: recv_buf 累积直到 ≥ 4 + frame_len
 *   STATE 3 — 解码帧: 判断数据类型 → 解码
 *
 * 返回: 1=已解码, 0=需要更多数据, -1=错误
 */
int protocol_decode(session_t *session, const uint8_t *buf, size_t len,
                    decoded_msg_t *out) {
    /* 向内部缓冲区追加新数据 (len=0 的调用仅解码已有缓冲) */
    if (len > 0) {
        if (session->recv_len + len > sizeof(session->recv_buf)) {
            LOG_ERROR("session %d recv buffer overflow (%zu + %zu)",
                      session->fd, (size_t)session->recv_len, len);
            return -1;
        }
        memcpy(session->recv_buf + session->recv_len, buf, len);
        session->recv_len += len;
    }

    /* 需要至少 4 字节才能读取帧长度 */
    if (session->recv_len < TCP_FRAME_HEADER_SIZE) {
        return 0;  /* 等待更多数据 */
    }

    /* 解析 4B 大端长度 */
    uint32_t frame_len;
    memcpy(&frame_len, session->recv_buf, 4);
    frame_len = ntohl(frame_len);

    /* 帧长度合理性检查 */
    if (frame_len > 100 * 1024 * 1024) {  /* 单帧最大 100MB */
        LOG_ERROR("session %d frame too large: %u bytes", session->fd, frame_len);
        return -1;
    }

    /* 检查是否收完整个帧 */
    size_t total_needed = TCP_FRAME_HEADER_SIZE + frame_len;
    if (session->recv_len < total_needed) {
        return 0;  /* 等待更多数据 */
    }

    /* 帧已完整 — 开始解码 */
    const uint8_t *frame_data = session->recv_buf + TCP_FRAME_HEADER_SIZE;

    /* 判断帧类型: 含 "abc" 魔数 → 数据帧, 否则 → 控制帧 */
    if (frame_len >= FRAME_MAGIC_LEN &&
        memcmp(frame_data, FRAME_MAGIC, FRAME_MAGIC_LEN) == 0) {

        /* ============================================================
         * 数据帧: "abc" + Zstd 压缩数据
         * ============================================================ */
        const uint8_t *zstd_data = frame_data + FRAME_MAGIC_LEN;
        size_t zstd_len = frame_len - FRAME_MAGIC_LEN;

        if (zstd_len == 0) {
            LOG_ERROR("session %d empty data frame", session->fd);
            goto decode_error;
        }

        /* Zstd 解压 */
        unsigned long long decomp_size = ZSTD_getFrameContentSize(zstd_data, zstd_len);
        if (decomp_size == ZSTD_CONTENTSIZE_ERROR || decomp_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            LOG_ERROR("session %d Zstd content size error", session->fd);
            goto decode_error;
        }
        if (decomp_size > BLOCK_SIZE_MAX + 4096) {
            LOG_ERROR("session %d decompressed size too large: %llu", session->fd, decomp_size);
            goto decode_error;
        }

        size_t actual_size = ZSTD_decompress(out->data, decomp_size,
                                             zstd_data, zstd_len);
        if (ZSTD_isError(actual_size)) {
            LOG_ERROR("session %d Zstd decompress failed: %s",
                      session->fd, ZSTD_getErrorName(actual_size));
            goto decode_error;
        }

        /* MsgPack 解码 — block_data 指向 data[] 内的子区间 */
        const uint8_t *block_ptr;
        uint32_t block_len;
        if (msgpack_decode_block(out->data, actual_size,
                                 &out->devno, &out->offset,
                                 &block_ptr, &block_len) != 0) {
            LOG_ERROR("session %d MsgPack decode failed", session->fd);
            goto decode_error;
        }

        /* 将 bin 数据移到 data[] 开头, 便于调用方使用 */
        if (block_ptr != out->data) {
            memmove(out->data, block_ptr, block_len);
        }
        out->data_len = block_len;
        out->type = DECODE_DATA_BLOCK;
        LOG_DEBUG("decoded data block: devno=%d offset=%lld size=%u",
                  out->devno, (long long)out->offset, out->data_len);

    } else {
        /* ============================================================
         * 控制帧: 原始字符串
         * ============================================================ */
        if (frame_len == CTL_INCREMENTAL_LEN &&
            memcmp(frame_data, CTL_INCREMENTAL, CTL_INCREMENTAL_LEN) == 0) {
            out->type = DECODE_CTL_INCREMENTAL;
            LOG_INFO("session %d: received ctlIncremental", session->fd);

        } else if (frame_len == CTL_END_INCREMENTAL_LEN &&
                   memcmp(frame_data, CTL_END_INCREMENTAL, CTL_END_INCREMENTAL_LEN) == 0) {
            out->type = DECODE_CTL_END_INCREMENTAL;
            LOG_INFO("session %d: received ctlEndIncremental", session->fd);

        } else {
            LOG_WARN("session %d unknown control frame (%u bytes): %.*s",
                     session->fd, frame_len, (int)MIN(frame_len, 64), frame_data);
            goto decode_error;
        }
    }

frame_done:
    /* 将已处理的数据移出缓冲区 (处理 TCP 粘包) */
    size_t remaining = session->recv_len - total_needed;
    if (remaining > 0) {
        memmove(session->recv_buf,
                session->recv_buf + total_needed, remaining);
    }
    session->recv_len = remaining;

    return 1;  /* 成功解码一条消息 */

decode_error:
    /* 错误时也消费已处理的数据 */
    remaining = session->recv_len - total_needed;
    if (remaining > 0) {
        memmove(session->recv_buf,
                session->recv_buf + total_needed, remaining);
    }
    session->recv_len = remaining;
    return -1;
}

const char *decode_error_string(int err) {
    switch (err) {
        case -1: return "protocol error";
        default:  return "unknown error";
    }
}
