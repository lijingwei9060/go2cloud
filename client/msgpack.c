/*
 * msgpack.c — MsgPack 编码模块实现
 *
 * 实现 MSVC std::string 风格的动态缓冲区 (SSO 不可用, 直接堆分配)。
 * 扩容策略: 初始 8KB → 不足时容量翻倍 → 直到满足需求。
 */

#include "msgpack.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* 内部写入辅助: 确保容量 ≥ written + need */
static int writer_ensure(msgpack_writer_t *w, size_t need) {
    if (w->capacity - w->written >= need) return 0;

    size_t new_cap = w->capacity ? w->capacity * 2 : MSGPACK_BUF_INITIAL_CAP;
    while (w->written + need > new_cap) {
        new_cap *= 2;
        if (new_cap > 0x8000000000000000ULL) {
            /* 溢出 — 使用精确需求值 */
            new_cap = w->written + need;
            break;
        }
    }

    uint8_t *new_buf = realloc(w->buf, new_cap);
    if (!new_buf) {
        LOG_ERROR("msgpack realloc(%zu) failed", new_cap);
        return -1;
    }
    w->buf      = new_buf;
    w->capacity = new_cap;
    return 0;
}

/* 内部写入原始字节 */
static int writer_append(msgpack_writer_t *w, const void *data, size_t len) {
    if (writer_ensure(w, len) != 0) return -1;
    memcpy(w->buf + w->written, data, len);
    w->written += len;
    return 0;
}

void msgpack_writer_init(msgpack_writer_t *w) {
    w->buf      = malloc(MSGPACK_BUF_INITIAL_CAP);
    w->written  = 0;
    w->capacity = w->buf ? MSGPACK_BUF_INITIAL_CAP : 0;
}

void msgpack_writer_free(msgpack_writer_t *w) {
    free(w->buf);
    w->buf      = NULL;
    w->written  = 0;
    w->capacity = 0;
}

void msgpack_writer_reset(msgpack_writer_t *w) {
    w->written = 0;
}

/*
 * 编码 MsgPack int32 (自选最短编码)。
 * 写入器写位置前进相应字节数。
 */
static int msgpack_write_int32(msgpack_writer_t *w, int32_t val) {
    if (val >= 0 && val <= 127) {
        /* fixint 正数: 1 字节 */
        uint8_t b = (uint8_t)val;
        return writer_append(w, &b, 1);
    }
    if (val >= -32 && val < 0) {
        /* fixint 负数: 1 字节 (符号扩展) */
        uint8_t b = (uint8_t)(val & 0xff);
        return writer_append(w, &b, 1);
    }
    if (val >= INT8_MIN && val <= INT8_MAX) {
        /* int8 */
        uint8_t buf[2] = { MSGPACK_INT8, (uint8_t)val };
        return writer_append(w, buf, 2);
    }
    if (val >= INT16_MIN && val <= INT16_MAX) {
        /* int16 (大端) */
        uint8_t buf[3] = { MSGPACK_INT16,
                           (uint8_t)(val >> 8), (uint8_t)(val & 0xff) };
        return writer_append(w, buf, 3);
    }
    /* int32 (大端) */
    uint8_t buf[5] = { MSGPACK_INT32,
                       (uint8_t)(val >> 24), (uint8_t)(val >> 16),
                       (uint8_t)(val >> 8),  (uint8_t)(val & 0xff) };
    return writer_append(w, buf, 5);
}

/*
 * 编码 MsgPack int64。
 * 当前偏移值可能超过 int32 范围, 始终使用 int64 格式。
 */
static int msgpack_write_int64(msgpack_writer_t *w, int64_t val) {
    uint8_t buf[9] = { MSGPACK_INT64,
                       (uint8_t)((uint64_t)val >> 56),
                       (uint8_t)((uint64_t)val >> 48),
                       (uint8_t)((uint64_t)val >> 40),
                       (uint8_t)((uint64_t)val >> 32),
                       (uint8_t)((uint64_t)val >> 24),
                       (uint8_t)((uint64_t)val >> 16),
                       (uint8_t)((uint64_t)val >> 8),
                       (uint8_t)((uint64_t)val & 0xff) };
    return writer_append(w, buf, 9);
}

/*
 * 编码 MsgPack bin (bin8 / bin16 / bin32 自选)。
 */
static int msgpack_write_bin(msgpack_writer_t *w,
                             const uint8_t *data, uint32_t len) {
    if (len < 256) {
        /* bin8: 0xc4 + 1B len */
        uint8_t header[2] = { MSGPACK_BIN8, (uint8_t)len };
        if (writer_append(w, header, 2) != 0) return -1;
    } else if (len < 65536) {
        /* bin16: 0xc5 + 2B len (大端) */
        uint8_t header[3] = { MSGPACK_BIN16,
                              (uint8_t)(len >> 8),
                              (uint8_t)(len & 0xff) };
        if (writer_append(w, header, 3) != 0) return -1;
    } else {
        /* bin32: 0xc6 + 4B len (大端) — 标准 1MB 块使用此路径 */
        uint8_t header[5] = { MSGPACK_BIN32,
                              (uint8_t)(len >> 24),
                              (uint8_t)(len >> 16),
                              (uint8_t)(len >> 8),
                              (uint8_t)(len & 0xff) };
        if (writer_append(w, header, 5) != 0) return -1;
    }
    return writer_append(w, data, len);
}

int msgpack_encode_block(msgpack_writer_t *w,
                         int32_t devno, int64_t offset,
                         const uint8_t *data, uint32_t data_len) {
    /* 重置写入位置 */
    w->written = 0;

    /*  0x93                              — fixmap(3) */
    if (writer_append(w,
                      (uint8_t[]){MSGPACK_FIXMAP_3}, 1) != 0) return -1;

    /*  0x00 <devno as int>               — key 0: devno */
    if (writer_append(w, (uint8_t[]){0x00}, 1) != 0) return -1;
    if (msgpack_write_int32(w, devno) != 0) return -1;

    /*  0x01 <offset as int64>            — key 1: offset */
    if (writer_append(w, (uint8_t[]){0x01}, 1) != 0) return -1;
    if (msgpack_write_int64(w, offset) != 0) return -1;

    /*  0x02 <data as bin8/16/32>         — key 2: data */
    if (writer_append(w, (uint8_t[]){0x02}, 1) != 0) return -1;
    if (msgpack_write_bin(w, data, data_len) != 0) return -1;

    LOG_DEBUG("msgpack encoded: devno=%d offset=%lld data_len=%u → %zu bytes",
              devno, (long long)offset, data_len, w->written);
    return 0;
}
