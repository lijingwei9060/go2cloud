/*
 * log.h / log.c — client-side log module
 *
 * Thread-safe logging with timestamps and levels.
 * Format: [LEVEL] YYYY/MM/DD HH:MM:SS file:line -| message
 *
 * Levels: 1=TRACE  2=DEBUG  3=INFO  4=WARN  5=ERROR
 */

#ifndef CLIENT_LOG_H
#define CLIENT_LOG_H

#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log level constants (use these for g_log_level / log_init) */
#define LOG_LEVEL_TRACE  1
#define LOG_LEVEL_DEBUG  2
#define LOG_LEVEL_INFO   3
#define LOG_LEVEL_WARN   4
#define LOG_LEVEL_ERROR  5

/* Current global log level */
extern int g_log_level;

/* Initialize logging: set level and optional file output (NULL = stderr) */
int log_init(int level, const char *filepath);

/* Close logging */
void log_close(void);

/* Core write function — use via convenience macros below */
void log_write(int level, const char *file, int line,
               const char *fmt, ...);

/*
 * Convenience macros: auto-fill __FILE__ and __LINE__.
 * Usage: LOG_INFO("connected to %s:%d", host, port);
 */
#define LOG_TRACE(fmt, ...)  log_write(LOG_LEVEL_TRACE, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)  log_write(LOG_LEVEL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)   log_write(LOG_LEVEL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   log_write(LOG_LEVEL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  log_write(LOG_LEVEL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_LOG_H */
