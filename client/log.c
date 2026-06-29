/*
 * log.c — client-side log module implementation
 */

#include "log.h"
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_log_lock;
#else
#include <pthread.h>
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

int    g_log_level = LOG_LEVEL_INFO;
static FILE *g_log_file = NULL;

int log_init(int level, const char *filepath) {
    g_log_level = level;
#ifdef _WIN32
    InitializeCriticalSection(&g_log_lock);
#endif
    if (filepath) {
        g_log_file = fopen(filepath, "a");
        if (!g_log_file) return -1;
    } else {
        g_log_file = stderr;
    }
    return 0;
}

void log_close(void) {
#ifdef _WIN32
    DeleteCriticalSection(&g_log_lock);
#endif
    if (g_log_file && g_log_file != stderr) {
        fclose(g_log_file);
    }
    g_log_file = NULL;
}

void log_write(int level, const char *file, int line,
               const char *fmt, ...) {
    if (level < g_log_level) return;

    /* Level tag strings */
    static const char *k_tags[] = {
        "",
        "[TRACE]",
        "[DEBUG]",
        "[INFO ]",
        "[WARN ]",
        "[ERROR]",
    };
    const char *tag = (level >= 1 && level <= 5) ? k_tags[level] : "[     ]";

    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now);
#else
    localtime_r(&now, &tm_buf);
#endif
    char time_str[32];
    snprintf(time_str, sizeof(time_str),
             "%04d/%02d/%02d %02d:%02d:%02d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    /* Extract filename (no path) */
    const char *fname = strrchr(file, '/');
    if (!fname) fname = strrchr(file, '\\');
    fname = fname ? fname + 1 : file;

    /* Format variadic args */
    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Thread-safe write */
#ifdef _WIN32
    EnterCriticalSection(&g_log_lock);
#else
    pthread_mutex_lock(&g_log_lock);
#endif

    fprintf(g_log_file, "%s %s %s:%d -| %s\n",
            tag, time_str, fname, line, msg);
    fflush(g_log_file);

#ifdef _WIN32
    LeaveCriticalSection(&g_log_lock);
#else
    pthread_mutex_unlock(&g_log_lock);
#endif
}
