/*
 * queue.c — 发送队列模块实现
 *
 * 环形缓冲区 + 互斥锁的线程安全队列。
 * 平台适配: Windows 用 CRITICAL_SECTION, Linux 用 pthread_mutex_t。
 * 存储为 opaque void* 以保持头文件不依赖平台头文件。
 */

#include "queue.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define MUTEX_T       CRITICAL_SECTION
#define MUTEX_INIT(m) InitializeCriticalSection((CRITICAL_SECTION *)(m))
#define MUTEX_LOCK(m) EnterCriticalSection((CRITICAL_SECTION *)(m))
#define MUTEX_UNLOCK(m) LeaveCriticalSection((CRITICAL_SECTION *)(m))
#define MUTEX_DEL(m)  DeleteCriticalSection((CRITICAL_SECTION *)(m))
#else
#include <pthread.h>
#define MUTEX_T       pthread_mutex_t
#define MUTEX_INIT(m) pthread_mutex_init((pthread_mutex_t *)(m), NULL)
#define MUTEX_LOCK(m) pthread_mutex_lock((pthread_mutex_t *)(m))
#define MUTEX_UNLOCK(m) pthread_mutex_unlock((pthread_mutex_t *)(m))
#define MUTEX_DEL(m)  pthread_mutex_destroy((pthread_mutex_t *)(m))
#endif

int queue_init(send_queue_t *q) {
    memset(q, 0, sizeof(*q));
    q->mutex = malloc(sizeof(MUTEX_T));
    if (!q->mutex) return -1;
    MUTEX_INIT(q->mutex);
    return 0;
}

void queue_destroy(send_queue_t *q) {
    if (q->mutex) {
        MUTEX_DEL(q->mutex);
        free(q->mutex);
        q->mutex = NULL;
    }
}

int queue_push(send_queue_t *q, int32_t devno, int64_t offset, uint64_t hash,
               const uint8_t *payload, size_t payload_len) {
    if (payload_len > QUEUE_ENTRY_MAX_PAYLOAD) {
        LOG_ERROR("queue_push: payload too large (%zu > %u)",
                  payload_len, (unsigned)QUEUE_ENTRY_MAX_PAYLOAD);
        return -1;
    }

    MUTEX_LOCK(q->mutex);

    if (q->count >= QUEUE_CAPACITY) {
        MUTEX_UNLOCK(q->mutex);
        return -1;  /* 队列满 */
    }

    int slot = q->head;
    queue_entry_t *e = &q->entries[slot];

    e->devno       = devno;
    e->offset      = offset;
    e->hash        = hash;
    e->payload_len = payload_len;
    memcpy(e->payload, payload, payload_len);
    e->pending     = 1;

    q->head = (slot + 1) % QUEUE_CAPACITY;
    q->count++;

    /* 背压检查 */
    if (q->count >= BACKPRESSURE_QUEUE_DEPTH) {
        q->backpressure = 1;
    }

    MUTEX_UNLOCK(q->mutex);

    if (q->backpressure) {
        LOG_DEBUG("queue_push: backpressure triggered (depth=%d)", q->count);
        return 2;
    }
    return 0;
}

int queue_pop(send_queue_t *q, queue_entry_t *out) {
    MUTEX_LOCK(q->mutex);

    if (q->count == 0) {
        MUTEX_UNLOCK(q->mutex);
        return -1;
    }

    int slot = q->tail;
    memcpy(out, &q->entries[slot], sizeof(*out));
    q->tail = (slot + 1) % QUEUE_CAPACITY;
    q->count--;

    /* 背压解除 */
    if (q->count < BACKPRESSURE_QUEUE_DEPTH) {
        q->backpressure = 0;
    }

    MUTEX_UNLOCK(q->mutex);
    return 0;
}

void queue_ack(send_queue_t *q, int32_t devno, int64_t offset) {
    MUTEX_LOCK(q->mutex);

    /* 线性扫描匹配 devno+offset (ACK 不频繁, 简单扫描即可) */
    for (int i = 0; i < QUEUE_CAPACITY; i++) {
        queue_entry_t *e = &q->entries[i];
        if (e->pending && e->devno == devno && e->offset == offset) {
            e->pending = 0;
            break;
        }
    }

    MUTEX_UNLOCK(q->mutex);
}

int queue_count(send_queue_t *q) {
    return q->count;
}

int queue_get_pending(send_queue_t *q, int32_t *devnos, int64_t *offsets,
                      int max_entries) {
    MUTEX_LOCK(q->mutex);

    int count = 0;
    for (int i = 0; i < QUEUE_CAPACITY && count < max_entries; i++) {
        queue_entry_t *e = &q->entries[i];
        if (e->pending) {
            devnos[count]  = e->devno;
            offsets[count] = e->offset;
            count++;
        }
    }

    MUTEX_UNLOCK(q->mutex);
    return count;
}

int queue_should_backpressure(send_queue_t *q) {
    return q->count >= BACKPRESSURE_QUEUE_DEPTH;
}
