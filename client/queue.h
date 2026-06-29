/*
 * queue.h — 发送队列模块
 *
 * 线程安全的环形缓冲区发送队列, 配合背压控制。
 * 生产者: 读块线程 (block_io) → 编码后入队
 * 消费者: 网络线程 → 出队并通过 wire_send() 发送
 *
 * 每个队列条目包含完整的已编码 MsgPack 块消息,
 * 以及元数据 (devno/offset) 供 ACK 匹配和重传使用。
 */

#ifndef CLIENT_QUEUE_H
#define CLIENT_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/* 单个队列条目的最大负载 (1MB MsgPack 编码后的上限) */
#define QUEUE_ENTRY_MAX_PAYLOAD  (0x100000 + 256)

/* 环形缓冲区容量 */
#define QUEUE_CAPACITY           256

/* 队列条目 */
typedef struct {
    int32_t  devno;                           /* 磁盘编号 */
    int64_t  offset;                          /* 块偏移 */
    uint64_t hash;                            /* 块哈希 (去重用) */
    uint8_t  payload[QUEUE_ENTRY_MAX_PAYLOAD]; /* MsgPack 编码数据 */
    size_t   payload_len;                     /* 实际载荷长度 */
    int      pending;                         /* 1 = 等待发送, 0 = 空闲 */
} queue_entry_t;

/* 环形缓冲区队列 */
typedef struct {
    queue_entry_t entries[QUEUE_CAPACITY];
    volatile int head;       /* 生产者写入位置 */
    volatile int tail;       /* 消费者读取位置 */
    volatile int count;      /* 当前条目数 */
    volatile int backpressure; /* 背压标志: 1 = 暂停生产 */
    void     *mutex;         /* 平台互斥锁 (opaque) */
} send_queue_t;

/* 初始化队列 */
int queue_init(send_queue_t *q);

/* 销毁队列 */
void queue_destroy(send_queue_t *q);

/*
 * 入队: 将编码后的块消息放入环形缓冲区。
 *
 * 如果队列深度 >= BACKPRESSURE_QUEUE_DEPTH, 返回 2 (背压通知)。
 * 如果队列满, 返回 -1 (生产者应重试)。
 * 成功返回 0。
 */
int queue_push(send_queue_t *q, int32_t devno, int64_t offset, uint64_t hash,
               const uint8_t *payload, size_t payload_len);

/*
 * 出队: 取出下一个待发送条目。
 *
 * 返回 0 成功, -1 队列空。
 * 调用者负责通过 wire_send() 发送条目的 payload,
 * 然后调用 queue_ack() 标记完成。
 */
int queue_pop(send_queue_t *q, queue_entry_t *out);

/*
 * 确认: 标记条目的 pending=0 (发送完成)。
 * devno/offset 用于匹配条目。
 */
void queue_ack(send_queue_t *q, int32_t devno, int64_t offset);

/* 获取队列中的条目数 */
int queue_count(send_queue_t *q);

/*
 * 获取所有挂起条目 (pending=1) 的 devno/offset 列表,
 * 用于重传扫描。返回条目数。
 */
int queue_get_pending(send_queue_t *q, int32_t *devnos, int64_t *offsets,
                      int max_entries);

/*
 * 检查是否应触发背压。
 * 条件: 队列深度 >= BACKPRESSURE_QUEUE_DEPTH
 * 返回 1 = 需要背压, 0 = 正常。
 */
int queue_should_backpressure(send_queue_t *q);

#endif /* CLIENT_QUEUE_H */
