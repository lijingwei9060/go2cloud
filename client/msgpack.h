/*
 * msgpack.h — MsgPack 编码模块
 *
 * 实现客户端 → 服务端消息的 MsgPack 编码。
 * 有线格式: fixmap(3) { 0: devno(int), 1: offset(int64), 2: data(bin32) }
 *
 * 编码依赖协议常量 (protocol.h) 中的:
 *   MSGPACK_FIXMAP_3, MSGPACK_BIN8/16/32, MSGPACK_INT8/16/32/64
 */

#ifndef CLIENT_MSGPACK_H
#define CLIENT_MSGPACK_H

#include <stdint.h>
#include <stddef.h>

/*
 * MsgPack 写入器。
 * 内部维护动态扩容缓冲区, 通过 write 函数指针追加数据。
 */
typedef struct msgpack_writer msgpack_writer_t;

struct msgpack_writer {
    uint8_t *buf;      /* 数据缓冲区 */
    size_t   written;  /* 已写入字节数 */
    size_t   capacity; /* 缓冲区容量 */
};

/* 初始化写入器 (初始容量 8KB) */
void msgpack_writer_init(msgpack_writer_t *w);

/* 释放写入器 */
void msgpack_writer_free(msgpack_writer_t *w);

/* 重置写入器 (清空内容, 保留缓冲区) */
void msgpack_writer_reset(msgpack_writer_t *w);

/*
 * 编码一个块消息。
 *
 * 输出格式:
 *   0x93                    — fixmap(3)
 *   0x00 <devno>            — key=0, value=磁盘编号
 *   0x01 <offset>           — key=1, value=字节偏移 (int64)
 *   0x02 <bin_header><data> — key=2, value=块数据 (bin8/16/32)
 *
 * 根据 data_len 自动选择最短 bin 编码:
 *   < 256      → bin8  (0xc4 + 1B len)
 *   < 65536    → bin16 (0xc5 + 2B len BE)
 *   >= 65536   → bin32 (0xc6 + 4B len BE)
 *
 * 返回: 0 成功, -1 失败
 */
int msgpack_encode_block(msgpack_writer_t *w,
                         int32_t devno, int64_t offset,
                         const uint8_t *data, uint32_t data_len);

#endif /* CLIENT_MSGPACK_H */
