/*
 * timer.h — 定时器模块
 *
 * 管理客户端异步定时任务:
 *   - 重传检查: 每 18 秒扫描未 ACK 块并重传
 *   - 重连检查: 每 20 秒检查并恢复断开的连接
 *   - 定期动作: 每 10 秒执行常规维护 (池回收、统计输出)
 *
 * 定时器通过时间戳比较触发, 无独立线程。
 * 调用者在主循环中调用 timer_check() 检查是否到触发时间。
 */

#ifndef CLIENT_TIMER_H
#define CLIENT_TIMER_H

#include <stdint.h>

/* 定时器事件类型 */
typedef enum {
    TIMER_NONE = 0,
    TIMER_RETRANSMIT,    /* 重传未确认块 */
    TIMER_RECONNECT,     /* 重建连接 */
    TIMER_ACTION,        /* 定期动作 */
    TIMER_INCREMENTAL,   /* 增量同步: 每 18s 从 live disk 重读并 hash 对比 */
    TIMER_COUNT
} timer_event_t;

/* 定时器管理器 */
typedef struct {
    uint64_t last_fire[TIMER_COUNT];  /* 各事件上次触发时间 (ms) */
    uint64_t interval[TIMER_COUNT];   /* 各事件间隔 (ms) */
} timer_mgr_t;

/* 初始化定时器管理器 */
void timer_init(timer_mgr_t *tm);

/*
 * 检查定时器是否就绪, 返回需要触发的第一个事件。
 *
 * now_ms: 当前系统时间 (毫秒, 从 GetTickCount64 / clock_gettime 获取)
 *
 * 返回 TIMER_NONE 表示无事件触发,
 * 其他返回第一个到期的事件类型。
 * 调用者应处理事件后再次调用以获取后续事件。
 */
timer_event_t timer_check(timer_mgr_t *tm, uint64_t now_ms);

/* 标记事件已处理 (重置其上次触发时间) */
void timer_reset(timer_mgr_t *tm, timer_event_t event);

/* 获取当前系统时间 (毫秒) */
uint64_t timer_now_ms(void);

#endif /* CLIENT_TIMER_H */
