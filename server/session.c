/*
 * session.c — 客户端会话管理实现
 */

#include "session.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static session_t *g_sessions[MAX_SESSIONS];
static int g_session_count = 0;

void session_mgr_init(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
    g_session_count = 0;
}

session_t *session_create(int fd, const char *remote_addr) {
    if (g_session_count >= MAX_SESSIONS) {
        LOG_WARN("session limit reached (%d)", MAX_SESSIONS);
        return NULL;
    }

    session_t *s = calloc(1, sizeof(session_t));
    if (!s) return NULL;

    s->fd = fd;
    s->connected_at = time(NULL);
    s->last_active = s->connected_at;
    s->expecting_frame = 0;
    s->recv_len = 0;
    snprintf(s->remote_addr, sizeof(s->remote_addr), "%s", remote_addr);

    g_sessions[g_session_count++] = s;
    LOG_INFO("session %d created from %s", fd, remote_addr);
    return s;
}

session_t *session_find(int fd) {
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i] && g_sessions[i]->fd == fd) {
            return g_sessions[i];
        }
    }
    return NULL;
}

void session_remove(int fd) {
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i] && g_sessions[i]->fd == fd) {
            LOG_INFO("session %d removed (%s, blocks=%llu, bytes=%llu)",
                     fd, g_sessions[i]->remote_addr,
                     (unsigned long long)g_sessions[i]->blocks_received,
                     (unsigned long long)g_sessions[i]->bytes_received);
            free(g_sessions[i]);
            /* 收缩数组 */
            memmove(&g_sessions[i], &g_sessions[i + 1],
                    (g_session_count - i - 1) * sizeof(session_t *));
            g_session_count--;
            return;
        }
    }
}

int session_count(void) {
    return g_session_count;
}

void session_foreach(int (*cb)(session_t *, void *), void *arg) {
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i]) {
            if (cb(g_sessions[i], arg)) break;
        }
    }
}

void session_touch(session_t *s) {
    s->last_active = time(NULL);
}
