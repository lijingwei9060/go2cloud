/*
 * queue.h — send queue module
 *
 * Thread-safe ring buffer send queue with backpressure control.
 * Producer: block read thread -> encode -> enqueue
 * Consumer: network thread -> dequeue -> wire_send()
 *
 * Payloads are heap-allocated to avoid stack overflow from 1MB entries.
 */

#ifndef CLIENT_QUEUE_H
#define CLIENT_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/* Max payload per entry (1MB encoded MsgPack upper bound) */
#define QUEUE_ENTRY_MAX_PAYLOAD  (0x100000 + 256)

/* Ring buffer capacity */
#define QUEUE_CAPACITY           256

/* Queue entry — payload is heap-allocated */
typedef struct {
    int32_t  devno;        /* disk number */
    int64_t  offset;       /* block offset */
    uint64_t hash;         /* block hash (dedup) */
    uint8_t *payload;      /* heap-allocated encoded data */
    size_t   payload_len;  /* actual payload length */
    int      pending;      /* 1 = waiting to send, 0 = idle/acked */
} queue_entry_t;

/* Ring buffer queue */
typedef struct {
    queue_entry_t entries[QUEUE_CAPACITY];
    volatile int head;
    volatile int tail;
    volatile int count;
    volatile int backpressure;
    void     *mutex;
} send_queue_t;

/* Initialize */
int queue_init(send_queue_t *q);

/* Destroy (frees all heap payloads) */
void queue_destroy(send_queue_t *q);

/*
 * Enqueue an encoded block message.
 * payload data is copied to heap.
 *
 * Returns: 0=ok, 2=backpressure triggered, -1=queue full
 */
int queue_push(send_queue_t *q, int32_t devno, int64_t offset, uint64_t hash,
               const uint8_t *payload, size_t payload_len);

/*
 * Dequeue next pending entry.
 * Caller gets a COPY of the entry (payload is still owned by queue).
 * Returns: 0=ok, -1=queue empty
 */
int queue_pop(send_queue_t *q, queue_entry_t *out);

/*
 * Acknowledge: marks entry pending=0 and frees its payload.
 * Matched by devno + offset.
 */
void queue_ack(send_queue_t *q, int32_t devno, int64_t offset);

/* Current entry count */
int queue_count(send_queue_t *q);

/*
 * Get all pending entry (devno, offset) pairs for retransmit scan.
 * Returns count of pending entries.
 */
int queue_get_pending(send_queue_t *q, int32_t *devnos, int64_t *offsets,
                      int max_entries);

/*
 * Check backpressure condition.
 * Returns 1 if depth >= BACKPRESSURE_QUEUE_DEPTH.
 */
int queue_should_backpressure(send_queue_t *q);

#endif /* CLIENT_QUEUE_H */
