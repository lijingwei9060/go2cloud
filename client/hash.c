/*
 * hash.c — 块哈希模块实现
 *
 * custom_hash — 自定义 64-bit 哈希, 基于 xxHash64 结构。
 *
 * 乘法常数: 0xc2b4d1b3d54d51c2  (= -0x3d4d51c2d82b14b1 在 2's complement)
 *            0x9e3779b586c04815  (= -0x61c8864e7a143579)
 * XOR 常数:  0x85ebca77c2b2ae63
 */

#include "hash.h"
#include "../include/protocol.h"

/* final mix: 雪崩混合函数 */
static inline uint64_t fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

uint64_t hash_block(const void *data, size_t len, uint64_t seed) {
    /* 短数据: 简单混合 */
    if (len < 32) {
        return (uint64_t)len + HASH_INIT_SHORT + seed;
    }

    const uint64_t *p = (const uint64_t *)data;
    const uint64_t *end = p + (len / sizeof(uint64_t)) - (len % sizeof(uint64_t) == 0 ? 4 : 0);

    /* 4 个独立状态 */
    uint64_t hash    = seed + HASH_INIT_A;
    uint64_t state_b = seed + HASH_INIT_B;
    uint64_t state_c = seed + HASH_INIT_C;
    uint64_t state_d = seed;

    /*
     * 主循环: 每次处理 32 字节 (4 × uint64)
     *
     * 对每条 64-bit 通道:
     *   1. hash += input * MUL
     *   2. hash = (hash << 31 | hash >> 33) * ROT   (31 位循环左旋)
     */
    while (p + 3 < end) {
        uint64_t k0 = p[0] * HASH_MUL;
        hash     += k0;
        hash      = ((hash << 31) | (hash >> 33)) * HASH_ROT;

        uint64_t k1 = p[1] * HASH_MUL;
        state_b  += k1;
        state_b   = ((state_b << 31) | (state_b >> 33)) * HASH_ROT;

        uint64_t k2 = p[2] * HASH_MUL;
        state_c  += k2;
        state_c   = ((state_c << 31) | (state_c >> 33)) * HASH_ROT;

        uint64_t k3 = p[3] * HASH_MUL;
        state_d  += k3;
        state_d   = ((state_d << 31) | (state_d >> 33)) * HASH_ROT;

        p += 4;
    }

    /*
     * 最终混合: 4 轮 FM → XOR → MUL → ADD
     * 每轮引入一个新状态, 确保所有输入位都影响输出
     */
    uint64_t result = HASH_XOR_CONST;
    result = (result ^ fmix64(hash))    * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_b)) * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_c)) * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_d)) * HASH_ROT + HASH_XOR_CONST;

    /* 混入长度 */
    result += (uint64_t)len;

    return result;
}
