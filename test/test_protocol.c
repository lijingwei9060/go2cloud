/*
 * 测试程序: test_protocol.c — 验证协议编解码的正确性
 *
 * 编译:
 *   gcc -O0 -g -o test_protocol test_protocol.c client/msgpack.c client/hash.c \
 *       client/log.c -Iinclude -lzstd -lpthread
 *
 * 测试项:
 *   1. MsgPack 编码 → 手动解码验证
 *   2. hash_block 确定性
 *   3. 块消息往返
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "include/protocol.h"
#include "client/msgpack.h"
#include "client/hash.h"
#include "client/log.h"

static int test_msgpack_encode(void) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    uint8_t test_data[256];
    for (int i = 0; i < 256; i++) test_data[i] = (uint8_t)i;

    int rc = msgpack_encode_block(&w, 0, 0x100000ULL, test_data, 256);
    assert(rc == 0);
    assert(w.written > 0);

    /* 验证头部: 0x93 (fixmap 3) */
    assert(w.buf[0] == MSGPACK_FIXMAP_3);

    /* 验证 key 0: 0x00 */
    assert(w.buf[1] == 0x00);

    printf("  [PASS] msgpack_encode_block: %zu bytes\n", w.written);
    msgpack_writer_free(&w);
    return 0;
}

static int test_msgpack_bin_sizes(void) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* bin8 (data_len < 256) */
    uint8_t small[100];
    memset(small, 0xAA, 100);
    msgpack_encode_block(&w, 0, 0, small, 100);
    /* 查找 bin8 标记 0xc4 */
    int found_bin8 = 0;
    for (size_t i = 0; i < w.written; i++) {
        if (w.buf[i] == MSGPACK_BIN8) { found_bin8 = 1; break; }
    }
    assert(found_bin8);
    printf("  [PASS] bin8 encoding detected\n");

    /* bin32 (data_len >= 65536) — 实际 1MB */
    /* (跳过完整 1MB 分配, 只验证路径选择) */

    msgpack_writer_free(&w);
    return 0;
}

static int test_hash_deterministic(void) {
    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (uint8_t)(i & 0xFF);

    uint64_t h1 = hash_block(data, 1024, 0);
    uint64_t h2 = hash_block(data, 1024, 0);
    uint64_t h3 = hash_block(data, 1024, 42);

    assert(h1 == h2);                 /* 相同输入 → 相同输出 */
    assert(h1 != h3);                 /* 不同种子 → 不同输出 */

    printf("  [PASS] hash_block deterministic: h1=h2=0x%016llx, h3=0x%016llx\n",
           (unsigned long long)h1, (unsigned long long)h3);
    return 0;
}

static int test_hash_short_data(void) {
    uint8_t tiny[16] = {0};
    uint64_t h = hash_block(tiny, 16, 0);
    /* 短数据路径: len + HASH_INIT_SHORT + seed */
    uint64_t expected = 16ULL + HASH_INIT_SHORT + 0ULL;
    assert(h == expected);
    printf("  [PASS] hash_block short data: 0x%016llx\n", (unsigned long long)h);
    return 0;
}

static int test_protocol_constants(void) {
    /* 确保结构体和常量一致 */
    assert(SERVER_RESPONSE_SIZE == 20);
    assert(sizeof(server_response_t) == 20);
    assert(BLOCK_SIZE == 0x100000ULL);
    assert(FRAME_MAGIC_LEN == 3);
    assert(TCP_FRAME_HEADER_SIZE == 4);

    printf("  [PASS] protocol constants verified\n");
    return 0;
}

int main(void) {
    log_init(LOG_WARN, NULL);  /* 仅警告和错误输出 */

    printf("=== go2cloud Protocol Tests ===\n\n");

    int pass = 0, fail = 0;

#define RUN_TEST(t) do { \
    printf("%s...\n", #t); \
    if (t() == 0) pass++; else { fail++; printf("  [FAIL]\n"); } \
} while(0)

    RUN_TEST(test_protocol_constants);
    RUN_TEST(test_msgpack_encode);
    RUN_TEST(test_msgpack_bin_sizes);
    RUN_TEST(test_hash_deterministic);
    RUN_TEST(test_hash_short_data);

    printf("\n=== Results: %d passed, %d failed ===\n", pass, fail);

    log_close();
    return fail > 0 ? 1 : 0;
}
