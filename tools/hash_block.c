/*
 * hash_block.c — 块哈希命令行工具 (Linux)
 *
 * 编译: gcc -O2 -o hash_block hash_block.c
 * 用法:
 *   ./hash_block <file>              计算整个文件的 hash
 *   ./hash_block <file> <offset> <size>  读取文件的指定偏移和长度
 *   dd if=/dev/sdb bs=1M skip=N count=1 2>/dev/null | ./hash_block -  (stdin)
 *
 * 算法与 client.exe 的 hash_block() 完全一致。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===== 常量 (与 include/protocol.h 一致) ===== */
#define HASH_MUL         0xc2b4d1b3d54d51c2ULL
#define HASH_ROT         0x9e3779b586c04815ULL
#define HASH_INIT_SHORT  0x27d4eb2f165667c5ULL
#define HASH_INIT_A      0x60ea27eeadc0b5d6ULL
#define HASH_INIT_B      0x61c8864e7a143579ULL
#define HASH_INIT_C      0xc2b2ae3d27d4eb4fULL
#define HASH_XOR_CONST   0x85ebca77c2b2ae63ULL

/* final mix: 雪崩混合 */
static inline uint64_t fmix64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

uint64_t hash_block(const void *data, size_t len, uint64_t seed) {
    if (len < 32) {
        return (uint64_t)len + HASH_INIT_SHORT + seed;
    }

    const uint64_t *p = (const uint64_t *)data;
    const uint64_t *end = p + (len / sizeof(uint64_t))
                        - (len % sizeof(uint64_t) == 0 ? 4 : 0);

    uint64_t hash    = seed + HASH_INIT_A;
    uint64_t state_b = seed + HASH_INIT_B;
    uint64_t state_c = seed + HASH_INIT_C;
    uint64_t state_d = seed;

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

    uint64_t result = HASH_XOR_CONST;
    result = (result ^ fmix64(hash))    * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_b)) * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_c)) * HASH_ROT + HASH_XOR_CONST;
    result = (result ^ fmix64(state_d)) * HASH_ROT + HASH_XOR_CONST;

    result += (uint64_t)len;
    return result;
}

int main(int argc, char *argv[]) {
    uint64_t seed = 0;
    const char *filepath;
    FILE *f;
    uint8_t *buf;
    long size, offset = 0;

    if (argc < 2) {
        fprintf(stderr, "Usage: hash_block <file> [offset] [size] [seed]\n");
        fprintf(stderr, "       hash_block -               read from stdin\n");
        fprintf(stderr, "  seed defaults to 0\n");
        fprintf(stderr, "  client.exe always uses seed=(offset & 0xFFFFFFFF)\n");
        fprintf(stderr, "  To match T_BLOCK hash: seed = offset & 0xFFFFFFFF\n");
        return 1;
    }

    filepath = argv[1];

    /* stdin 模式 */
    if (strcmp(filepath, "-") == 0) {
        size_t cap = 1 << 20;
        buf = malloc(cap);
        size = 0;
        for (;;) {
            if ((size_t)size >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            size_t n = fread(buf + size, 1, cap - size, stdin);
            if (n == 0) break;
            size += n;
        }
        if (argc >= 3) seed = (uint64_t)atoll(argv[argc - 1]);
        uint64_t h = hash_block(buf, size, seed);
        printf("0x%016llx  (size=%ld, seed=0x%llx)\n",
               (unsigned long long)h, size, (unsigned long long)seed);
        free(buf);
        return 0;
    }

    /* 文件模式 */
    f = fopen(filepath, "rb");
    if (!f) { perror(filepath); return 1; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);

    if (argc >= 4) {
        offset = atol(argv[2]);
        size   = atol(argv[3]);
        if (offset + size > file_size) {
            fprintf(stderr, "offset+size exceeds file size (%ld)\n", file_size);
            fclose(f);
            return 1;
        }
    } else {
        size = file_size;
    }
    if (argc >= 5) {
        seed = (uint64_t)atoll(argv[4]);
    }

    buf = malloc(size);
    if (!buf) { perror("malloc"); fclose(f); return 1; }

    fseek(f, offset, SEEK_SET);
    fread(buf, 1, size, f);
    fclose(f);

    uint64_t h = hash_block(buf, size, seed);
    printf("0x%016llx  (offset=%ld, size=%ld, seed=0x%llx)\n",
           (unsigned long long)h, offset, size, (unsigned long long)seed);

    free(buf);
    return 0;
}
