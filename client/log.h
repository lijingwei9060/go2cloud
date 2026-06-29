/*
 * log.h / log.c — 服务端日志模块
 *
 * 提供带时间戳和级别的线程安全日志输出。
 * 日志格式: [LEVEL] YYYY/MM/DD HH:MM:SS file:line -| message
 *
 * 级别 (从低到高):
 *   1 = TRACE   2 = DEBUG   3 = INFO   4 = WARN   5 = ERROR
 */

#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include <stdio.h>
#include <time.h>

/* 日志级别 */
#define LOG_TRACE  1
#define LOG_DEBUG  2
#define LOG_INFO   3
#define LOG_WARN   4
#define LOG_ERROR  5

/* 当前全局日志级别, 低于此级别的日志被过滤 */
extern int g_log_level;

/* 设置日志级别并打开日志文件 (传 NULL 输出到 stderr) */
int log_init(int level, const char *filepath);

/* 关闭日志 */
void log_close(void);

/* 核心日志函数 — 通常通过下面的宏调用 */
void log_write(int level, const char *file, int line,
               const char *fmt, ...);

/*
 * 便捷宏: 自动填入 __FILE__ 和 __LINE__。
 * 用法: LOG_INFO("connected to %s:%d", host, port);
 */
#define LOG_TRACE(fmt, ...)  log_write(LOG_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  log_write(LOG_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)   log_write(LOG_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   log_write(LOG_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  log_write(LOG_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif /* SERVER_LOG_H */
