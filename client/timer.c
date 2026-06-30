/*
 * timer.c — 定时器模块实现
 */

#include "timer.h"
#include "../include/protocol.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

void timer_init(timer_mgr_t *tm) {
    uint64_t now = timer_now_ms();
    tm->interval[TIMER_RETRANSMIT]   = RETRANSMIT_TIMER_MS;
    tm->interval[TIMER_RECONNECT]    = RECONNECT_TIMER_MS;
    tm->interval[TIMER_ACTION]       = ACTION_TIMER_MS;
    tm->interval[TIMER_INCREMENTAL]  = RETRANSMIT_TIMER_MS;  /* 18s */

    /* 错开首次触发: 连接建立后立即检查, 动作延迟 5 秒开始,
     * 增量延迟 18 秒后开始 (全量同步完成后再激活) */
    tm->last_fire[TIMER_RETRANSMIT]  = now;
    tm->last_fire[TIMER_RECONNECT]   = now;
    tm->last_fire[TIMER_ACTION]      = now + 5000 - ACTION_TIMER_MS;
    tm->last_fire[TIMER_INCREMENTAL] = now;  /* reset when full sync done */
}

uint64_t timer_now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

timer_event_t timer_check(timer_mgr_t *tm, uint64_t now_ms) {
    /* 按优先级检查: 重连 > 重传 > 动作 */
    for (int i = 1; i < TIMER_COUNT; i++) {
        if (now_ms - tm->last_fire[i] >= tm->interval[i]) {
            return (timer_event_t)i;
        }
    }
    return TIMER_NONE;
}

void timer_reset(timer_mgr_t *tm, timer_event_t event) {
    tm->last_fire[event] = timer_now_ms();
}
