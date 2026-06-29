/*
 * queue.c — send queue module implementation
 *
 * Ring buffer + mutex for thread safety.
 * Each entry's payload is heap-allocated, freed on ack or destroy.
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
        /* Free all heap payloads */
        for (int i = 0; i < QUEUE_CAPACITY; i++) {
            if (q->entries[i].payload) {
                free(q->entries[i].payload);
                q->entries[i].payload = NULL;
            }
        }
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
        return -1;
    }

    int slot = q->head;
    queue_entry_t *e = &q->entries[slot];

    /* Allocate and copy payload */
    uint8_t *copy = malloc(payload_len);
    if (!copy) {
        MUTEX_UNLOCK(q->mutex);
        return -1;
    }
    memcpy(copy, payload, payload_len);

    /* Free old payload if any (shouldn't happen with ring buffer) */
    if (e->payload) free(e->payload);

    e->devno       = devno;
    e->offset      = offset;
    e->hash        = hash;
    e->payload     = copy;
    e->payload_len = payload_len;
    e->pending     = 1;

    q->head = (slot + 1) % QUEUE_CAPACITY;
    q->count++;

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
    /* Ownership of payload REMAINS with queue — caller reads but doesn't free */
    q->tail = (slot + 1) % QUEUE_CAPACITY;
    q->count--;

    if (q->count < BACKPRESSURE_QUEUE_DEPTH) {
        q->backpressure = 0;
    }

    MUTEX_UNLOCK(q->mutex);
    return 0;
}

void queue_ack(send_queue_t *q, int32_t devno, int64_t offset) {
    MUTEX_LOCK(q->mutex);

    for (int i = 0; i < QUEUE_CAPACITY; i++) {
        queue_entry_t *e = &q->entries[i];
        if (e->pending && e->devno == devno && e->offset == offset) {
            e->pending = 0;
            if (e->payload) {
                free(e->payload);
                e->payload = NULL;
            }
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
