/*
 * test_protocol.c — go2cloud protocol unit tests
 *
 * Tests: protocol constants, MsgPack encode, hash_block, send queue.
 *
 * Build (x64 Native Tools Command Prompt):
 *   cl /O2 /utf-8 /Fe:test_protocol.exe test\test_protocol.c client\msgpack.c client\hash.c client\log.c client\queue.c /Iinclude /Iclient /Fotest\ /link /LIBPATH:D:\vcpkg\installed\x64-windows\lib
 *
 * Run:
 *   test_protocol.exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -- include project headers -- */
#include "../include/protocol.h"
#include "../client/msgpack.h"
#include "../client/hash.h"
#include "../client/queue.h"

/* -- test framework -- */
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name)  static void test_##name(void)
#define RUN(name)   do { \
    printf("  %-50s", #name); \
    g_passed++; \
    test_##name(); \
    printf(" PASSED\n"); \
} while(0)

#define CHECK(cond, msg)  do { \
    if (!(cond)) { \
        printf("\n    FAIL: %s\n", msg); \
        g_failed++; g_passed--; \
        return; \
    } \
} while(0)

#define CHECK_EQ(a, b, msg)  do { \
    if ((a) != (b)) { \
        printf("\n    FAIL: %s (expected %llu, got %llu)\n", \
               msg, (unsigned long long)(b), (unsigned long long)(a)); \
        g_failed++; g_passed--; \
        return; \
    } \
} while(0)

/* ================================================================
 * 1. Protocol constants
 * ================================================================ */

TEST(protocol_constants) {
    CHECK_EQ(sizeof(server_response_t), 20, "server_response_t size");
    CHECK_EQ(SERVER_RESPONSE_SIZE, 20, "SERVER_RESPONSE_SIZE");
    CHECK_EQ(BLOCK_SIZE, 0x100000ULL, "BLOCK_SIZE == 1MB");
    CHECK_EQ(FRAME_MAGIC_LEN, 3, "FRAME_MAGIC_LEN");
    CHECK_EQ(TCP_FRAME_HEADER_SIZE, 4, "TCP_FRAME_HEADER_SIZE");
    CHECK_EQ(MSGPACK_FIXMAP_3, 0x93, "MSGPACK_FIXMAP_3");
    CHECK_EQ(MSGPACK_BIN8,  0xc4, "MSGPACK_BIN8");
    CHECK_EQ(MSGPACK_BIN16, 0xc5, "MSGPACK_BIN16");
    CHECK_EQ(MSGPACK_BIN32, 0xc6, "MSGPACK_BIN32");
    CHECK_EQ(MSGPACK_INT8,  0xd0, "MSGPACK_INT8");
    CHECK_EQ(MSGPACK_INT16, 0xd1, "MSGPACK_INT16");
    CHECK_EQ(MSGPACK_INT32, 0xd2, "MSGPACK_INT32");
    CHECK_EQ(MSGPACK_INT64, 0xd3, "MSGPACK_INT64");
}

/* ================================================================
 * 2. MsgPack encoder — int32 encoding
 * ================================================================ */

/* Verify the written buffer contains expected bytes */
static int verify_bytes(const uint8_t *buf, const uint8_t *expected, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != expected[i]) return 0;
    }
    return 1;
}

TEST(msgpack_fixint_positive) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* devno=0: fixint 0x00 */
    uint8_t data = 0xAA;
    int rc = msgpack_encode_block(&w, 0/*devno*/, 0/*offset*/, &data, 1);

    CHECK_EQ(rc, 0, "encode return");
    CHECK(w.written > 10, "output not empty");

    /* Header: 0x93 (fixmap 3) */
    CHECK_EQ(w.buf[0], 0x93, "fixmap header");
    CHECK_EQ(w.buf[1], 0x00, "key 0 (devno)");
    CHECK_EQ(w.buf[2], 0x00, "devno=0 as fixint 0");
    CHECK_EQ(w.buf[3], 0x01, "key 1 (offset)");

    msgpack_writer_free(&w);
}

TEST(msgpack_devno_127_boundary) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* devno=127: still fixint */
    uint8_t data[10];
    memset(data, 0xBB, 10);
    int rc = msgpack_encode_block(&w, 127, 0, data, 10);

    CHECK_EQ(rc, 0, "encode return");
    /* fixint 127 = 0x7F */
    CHECK_EQ(w.buf[2], 0x7F, "devno 127 as 0x7F");

    msgpack_writer_free(&w);
}

TEST(msgpack_devno_128_not_fixint) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* devno=128: int8 range is -128..127, so 128 needs int16 (0xd1) */
    uint8_t data[10];
    memset(data, 0xBB, 10);
    int rc = msgpack_encode_block(&w, 128, 0, data, 10);

    CHECK_EQ(rc, 0, "encode return");
    CHECK_EQ(w.buf[2], MSGPACK_INT16, "int16 marker (128 > int8 max 127)");
    CHECK_EQ(w.buf[3], 0x00, "value high byte");
    CHECK_EQ(w.buf[4], 0x80, "value low byte");

    msgpack_writer_free(&w);
}

TEST(msgpack_devno_negative) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* devno=-1: fixint negative */
    uint8_t data[10];
    memset(data, 0xBB, 10);
    int rc = msgpack_encode_block(&w, -1, 0, data, 10);

    CHECK_EQ(rc, 0, "encode return");
    /* -1 in fixint negative = 0xFF */
    CHECK_EQ(w.buf[2], 0xFF, "devno -1 as 0xFF");

    msgpack_writer_free(&w);
}

/* ================================================================
 * 3. MsgPack bin encoding sizes
 * ================================================================ */

static int find_byte_in_buf(const uint8_t *buf, size_t len, uint8_t target) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == target) return 1;
    }
    return 0;
}

TEST(msgpack_bin8_small_data) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    uint8_t data[100];
    memset(data, 0xCC, 100);
    int rc = msgpack_encode_block(&w, 0, 0, data, 100);

    CHECK_EQ(rc, 0, "encode return");
    /* bin8 marker should appear somewhere after key 2 (0x02) */
    CHECK(find_byte_in_buf(w.buf, w.written, MSGPACK_BIN8), "bin8 marker found");

    msgpack_writer_free(&w);
}

TEST(msgpack_bin32_large_data) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    /* 1MB data — bin32 path */
    size_t big_size = BLOCK_SIZE;
    uint8_t *data = malloc(big_size);
    CHECK(data != NULL, "malloc 1MB");
    memset(data, 0xDD, big_size);

    int rc = msgpack_encode_block(&w, 0, 0x100000ULL, data, (uint32_t)big_size);
    CHECK_EQ(rc, 0, "encode return");
    CHECK(w.written > BLOCK_SIZE, "output >= 1MB");
    /* Should contain bin32 marker (0xc6) */
    CHECK(find_byte_in_buf(w.buf, w.written, MSGPACK_BIN32), "bin32 marker found");

    free(data);
    msgpack_writer_free(&w);
}

/* ================================================================
 * 4. MsgPack writer life cycle
 * ================================================================ */

TEST(msgpack_writer_reset) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    uint8_t data[50];
    memset(data, 0xEE, 50);

    /* First encode */
    msgpack_encode_block(&w, 0, 0, data, 50);
    size_t first_len = w.written;
    CHECK(first_len > 0, "first encode produced output");

    /* Reset and encode again */
    msgpack_writer_reset(&w);
    CHECK_EQ(w.written, 0, "written reset to 0");
    msgpack_encode_block(&w, 0, 0, data, 50);
    CHECK_EQ(w.written, first_len, "second encode same length");

    msgpack_writer_free(&w);
}

TEST(msgpack_multiple_encodes) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    uint8_t data[256];
    for (int i = 0; i < 256; i++) data[i] = (uint8_t)i;

    /* Encode 10 blocks sequentially */
    for (int n = 0; n < 10; n++) {
        msgpack_writer_reset(&w);
        int rc = msgpack_encode_block(&w, n, n * BLOCK_SIZE, data, 256);
        CHECK_EQ(rc, 0, "encode return");
    }

    msgpack_writer_free(&w);
}

/* ================================================================
 * 5. Hash function
 * ================================================================ */

TEST(hash_deterministic) {
    uint8_t data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (uint8_t)(i & 0xFF);

    uint64_t h1 = hash_block(data, 1024, 0);
    uint64_t h2 = hash_block(data, 1024, 0);
    CHECK_EQ(h1, h2, "same input -> same hash");

    /* different seed -> different hash */
    uint64_t h3 = hash_block(data, 1024, 42);
    CHECK(h1 != h3, "different seed -> different hash");
}

TEST(hash_short_data) {
    uint8_t tiny[16];
    memset(tiny, 0, 16);

    uint64_t h = hash_block(tiny, 16, 0);
    uint64_t expected = 16ULL + HASH_INIT_SHORT + 0ULL;
    CHECK_EQ(h, expected, "short data formula: len + INIT_SHORT + seed");
}

TEST(hash_different_data) {
    uint8_t a[256], b[256];
    for (int i = 0; i < 256; i++) {
        a[i] = (uint8_t)i;
        b[i] = (uint8_t)(255 - i);
    }

    uint64_t ha = hash_block(a, 256, 0);
    uint64_t hb = hash_block(b, 256, 0);
    CHECK(ha != hb, "different data -> different hash");
}

TEST(hash_empty_input) {
    uint8_t dummy = 0;
    uint64_t h = hash_block(&dummy, 0, 0);
    uint64_t expected = 0ULL + HASH_INIT_SHORT + 0ULL;
    CHECK_EQ(h, expected, "len=0: 0 + INIT_SHORT + 0");
}

TEST(hash_boundary_32) {
    /* len=31: short path; len=32: long path */
    uint8_t data31[31], data32[32];
    memset(data31, 0xAA, 31);
    memset(data32, 0xAA, 32);

    uint64_t h31 = hash_block(data31, 31, 0);
    uint64_t h32 = hash_block(data32, 32, 0);

    /* Different lengths should produce different hashes */
    CHECK(h31 != h32, "31 vs 32 bytes produce different hashes");
}

/* ================================================================
 * 6. Send queue
 * ================================================================ */

TEST(queue_push_pop_order) {
    send_queue_t q;
    queue_init(&q);

    uint8_t payload[100];
    memset(payload, 0x11, 100);

    /* Push 3 items */
    queue_push(&q, 0, 0x000000ULL, 0xAAA, payload, 100);
    queue_push(&q, 0, 0x100000ULL, 0xBBB, payload, 100);
    queue_push(&q, 0, 0x200000ULL, 0xCCC, payload, 100);

    CHECK_EQ(queue_count(&q), 3, "count after 3 pushes");

    /* Pop in order */
    queue_entry_t e;
    CHECK_EQ(queue_pop(&q, &e), 0, "pop 1");
    CHECK_EQ(e.offset, 0x000000ULL, "pop 1 offset");
    CHECK_EQ(e.hash, 0xAAA, "pop 1 hash");

    CHECK_EQ(queue_pop(&q, &e), 0, "pop 2");
    CHECK_EQ(e.offset, 0x100000ULL, "pop 2 offset");
    CHECK_EQ(e.hash, 0xBBB, "pop 2 hash");

    CHECK_EQ(queue_pop(&q, &e), 0, "pop 3");
    CHECK_EQ(e.offset, 0x200000ULL, "pop 3 offset");
    CHECK_EQ(e.hash, 0xCCC, "pop 3 hash");

    CHECK_EQ(queue_count(&q), 0, "count after 3 pops");
    CHECK_EQ(queue_pop(&q, &e), -1, "pop empty returns -1");

    queue_destroy(&q);
}

TEST(queue_ack) {
    send_queue_t q;
    queue_init(&q);

    uint8_t payload[100];
    memset(payload, 0x22, 100);

    queue_push(&q, 1, 0x500000ULL, 0x111, payload, 100);
    queue_push(&q, 1, 0x600000ULL, 0x222, payload, 100);
    CHECK_EQ(queue_count(&q), 2, "count 2");

    /* Ack the first item */
    queue_ack(&q, 1, 0x500000ULL);

    /* Verify pending list doesn't include acked item */
    int32_t  devnos[10];
    int64_t  offsets[10];
    int n = queue_get_pending(&q, devnos, offsets, 10);
    CHECK_EQ(n, 1, "1 pending after ack");
    CHECK_EQ(offsets[0], 0x600000ULL, "remaining offset");

    /* Ack the second */
    queue_ack(&q, 1, 0x600000ULL);
    n = queue_get_pending(&q, devnos, offsets, 10);
    CHECK_EQ(n, 0, "0 pending after both acked");

    queue_destroy(&q);
}

TEST(queue_pop_then_ack) {
    send_queue_t q;
    queue_init(&q);

    uint8_t payload[100];
    memset(payload, 0x33, 100);

    queue_push(&q, 2, 0xA00000ULL, 0xDEAD, payload, 100);
    queue_push(&q, 2, 0xB00000ULL, 0xBEEF, payload, 100);

    /* Pop first entry (it's now "in flight") */
    queue_entry_t e;
    queue_pop(&q, &e);
    CHECK_EQ(e.hash, 0xDEAD, "popped correct item");

    /* Ack the popped entry */
    queue_ack(&q, 2, 0xA00000ULL);

    /* Pop second */
    queue_pop(&q, &e);
    CHECK_EQ(e.hash, 0xBEEF, "popped second item");

    queue_destroy(&q);
}

TEST(queue_backpressure) {
    send_queue_t q;
    queue_init(&q);

    uint8_t payload[100];
    memset(payload, 0x44, 100);

    /* Push up to backpressure threshold - 1 */
    for (int i = 0; i < BACKPRESSURE_QUEUE_DEPTH - 1; i++) {
        int rc = queue_push(&q, 0, (int64_t)i * BLOCK_SIZE, (uint64_t)i, payload, 100);
        CHECK_EQ(rc, 0, "push before threshold");
    }

    CHECK(!queue_should_backpressure(&q), "no backpressure before threshold");

    /* One more push should trigger backpressure */
    int rc = queue_push(&q, 0, (int64_t)(BACKPRESSURE_QUEUE_DEPTH - 1) * BLOCK_SIZE,
                         BACKPRESSURE_QUEUE_DEPTH, payload, 100);
    CHECK_EQ(rc, 2, "push returns 2 (backpressure)");
    CHECK(queue_should_backpressure(&q), "backpressure active");

    /* Pop one to release backpressure */
    queue_entry_t e;
    queue_pop(&q, &e);
    CHECK(!queue_should_backpressure(&q), "backpressure released after pop");

    queue_destroy(&q);
}

/* ================================================================
 * 7. Block message round-trip verification
 * ================================================================ */

TEST(msgpack_round_trip_structure) {
    msgpack_writer_t w;
    msgpack_writer_init(&w);

    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i & 0xFF);

    int32_t  devno    = 3;
    int64_t  offset   = 0x12345678ULL;
    uint32_t data_len = 512;

    int rc = msgpack_encode_block(&w, devno, offset, data, data_len);
    CHECK_EQ(rc, 0, "encode success");

    /* Verify structure manually:
     *   0x93              — fixmap(3)
     *   0x00              — key 0
     *   <devno: 0x03>     — fixint 3
     *   0x01              — key 1
     *   0xd3 <8B offset>  — int64
     *   0x02              — key 2
     *   0xc4 0x80 0x02   — bin8 (512 = 0x0200)
     *   <512 bytes data>
     */

    size_t pos = 0;
    CHECK_EQ(w.buf[pos++], 0x93, "fixmap(3)");

    /* Key 0: devno */
    CHECK_EQ(w.buf[pos++], 0x00, "key 0");
    CHECK_EQ(w.buf[pos++], 0x03, "devno=3 fixint");

    /* Key 1: offset (int64) */
    CHECK_EQ(w.buf[pos++], 0x01, "key 1");
    CHECK_EQ(w.buf[pos++], MSGPACK_INT64, "int64 marker");
    /* 8 bytes big-endian offset 0x12345678 */
    pos += 7; /* skip first 7 bytes of offset */
    CHECK_EQ(w.buf[pos++], 0x78, "offset LSB");

    /* Key 2: bin data */
    CHECK_EQ(w.buf[pos++], 0x02, "key 2");
    /* bin16: 0xc5 + 2B len (512 = 0x0200) */
    CHECK_EQ(w.buf[pos++], MSGPACK_BIN16, "bin16 marker");
    CHECK_EQ(w.buf[pos++], 0x02, "len hi byte");
    CHECK_EQ(w.buf[pos++], 0x00, "len lo byte (=512)");

    /* Verify data bytes */
    CHECK_EQ(w.buf[pos], data[0], "data[0] matches");
    CHECK_EQ(w.buf[pos + 511], data[511], "data[511] matches");

    msgpack_writer_free(&w);
}

/* ================================================================
 * Entry point
 * ================================================================ */

int main(void) {
    printf("=== go2cloud Protocol Unit Tests ===\n\n");

    /* 1. Protocol constants */
    printf("[1] Protocol constants\n");
    RUN(protocol_constants);

    /* 2. MsgPack int32 encoding */
    printf("\n[2] MsgPack int32 encoding\n");
    RUN(msgpack_fixint_positive);
    RUN(msgpack_devno_127_boundary);
    RUN(msgpack_devno_128_not_fixint);
    RUN(msgpack_devno_negative);

    /* 3. MsgPack bin encoding */
    printf("\n[3] MsgPack bin encoding\n");
    RUN(msgpack_bin8_small_data);
    RUN(msgpack_bin32_large_data);

    /* 4. MsgPack writer lifecycle */
    printf("\n[4] MsgPack writer lifecycle\n");
    RUN(msgpack_writer_reset);
    RUN(msgpack_multiple_encodes);

    /* 5. Hash function */
    printf("\n[5] Hash function\n");
    RUN(hash_deterministic);
    RUN(hash_short_data);
    RUN(hash_different_data);
    RUN(hash_empty_input);
    RUN(hash_boundary_32);

    /* 6. Send queue */
    printf("\n[6] Send queue\n");
    RUN(queue_push_pop_order);
    RUN(queue_ack);
    RUN(queue_pop_then_ack);
    RUN(queue_backpressure);

    /* 7. Round-trip */
    printf("\n[7] Block message round-trip\n");
    RUN(msgpack_round_trip_structure);

    /* Summary */
    int total = g_passed + g_failed;
    printf("\n========================================\n");
    printf("Results: %d/%d passed", g_passed, total);
    if (g_failed > 0) {
        printf(", %d FAILED", g_failed);
    }
    printf("\n========================================\n");

    return g_failed > 0 ? 1 : 0;
}
