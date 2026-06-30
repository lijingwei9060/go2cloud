/*
 * sqlite.c — SQLite 块跟踪模块实现
 *
 * 使用 WAL 模式以获得更好的并发性能。
 * 所有 SQL 操作在每次调用中准备→执行→清理完成。
 */

#include "sqlite.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sqlite3.h>

/* 内部结构: 包装 sqlite3* */
struct sqlite_db {
    sqlite3    *handle;
    char        remote_id[256];
};

/* 预备的 SQL 语句 (不缓存, 每次准备) */
#define SQL_CREATE_TABLE \
    "CREATE TABLE IF NOT EXISTS T_BLOCK(" \
    "  devno     INTEGER," \
    "  offset    INTEGER," \
    "  size      INTEGER," \
    "  hash      INTEGER," \
    "  ack       INTEGER DEFAULT 0," \
    "  remote_id TEXT" \
    ");" \
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_block ON T_BLOCK(devno, offset);"

#define SQL_LOOKUP \
    "SELECT hash FROM T_BLOCK WHERE devno=? AND offset=?"

#define SQL_CHECK_ACK \
    "SELECT ack FROM T_BLOCK WHERE devno=? AND offset=?"

#define SQL_UPSERT \
    "INSERT OR REPLACE INTO T_BLOCK(devno, offset, size, hash, ack, remote_id) " \
    "VALUES(?, ?, ?, ?, ?, ?)"

#define SQL_MARK_ACK \
    "UPDATE T_BLOCK SET ack=1 WHERE devno=? AND offset=?"

#define SQL_GET_UNACKED \
    "SELECT devno, offset FROM T_BLOCK WHERE ack=0 AND remote_id=? " \
    "ORDER BY offset LIMIT ?"

sqlite_db_t *sqlite_open(const char *db_path) {
    sqlite3 *handle = NULL;
    int rc = sqlite3_open(db_path, &handle);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_open(%s) failed: %s", db_path, sqlite3_errmsg(handle));
        sqlite3_close(handle);
        return NULL;
    }

    /* 启用 WAL 模式 */
    char *err = NULL;
    rc = sqlite3_exec(handle, "PRAGMA journal_mode=WAL", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_WARN("sqlite: WAL mode failed: %s", err);
        sqlite3_free(err);
    }

    /* 创建表 */
    rc = sqlite3_exec(handle, SQL_CREATE_TABLE, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite: CREATE TABLE failed: %s", err);
        sqlite3_free(err);
        sqlite3_close(handle);
        return NULL;
    }

    sqlite_db_t *db = malloc(sizeof(*db));
    if (!db) {
        sqlite3_close(handle);
        return NULL;
    }
    db->handle = handle;
    db->remote_id[0] = '\0';

    LOG_INFO("sqlite: opened %s (WAL mode)", db_path);
    return db;
}

void sqlite_close(sqlite_db_t *db) {
    if (db) {
        sqlite3_close(db->handle);
        free(db);
    }
}

int64_t sqlite_total_acked_bytes(sqlite_db_t *db) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COALESCE(SUM(size), 0) FROM T_BLOCK WHERE ack=1";
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_total_acked: prepare failed: %s", sqlite3_errmsg(db->handle));
        return -1;
    }

    int64_t total = 0;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        total = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return total;
}

int sqlite_clear_all_blocks(sqlite_db_t *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db->handle, "DELETE FROM T_BLOCK", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_clear_all: %s", err);
        sqlite3_free(err);
        return -1;
    }
    LOG_INFO("sqlite: T_BLOCK cleared");
    return 0;
}

void sqlite_set_remote_id(sqlite_db_t *db, const char *remote_id) {
    if (db && remote_id) {
        strncpy(db->remote_id, remote_id, sizeof(db->remote_id) - 1);
        db->remote_id[sizeof(db->remote_id) - 1] = '\0';
    }
}

int sqlite_block_lookup(sqlite_db_t *db, int32_t devno, int64_t offset,
                        uint64_t *out_hash) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_LOOKUP, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_lookup: prepare failed: %s", sqlite3_errmsg(db->handle));
        return -2;
    }

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_hash = (uint64_t)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return 0;  /* 找到 */
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? -1 : -2;  /* 未找到 或 错误 */
}

int sqlite_block_acked(sqlite_db_t *db, int32_t devno, int64_t offset) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_CHECK_ACK, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int acked = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return acked ? 1 : 0;
    }

    sqlite3_finalize(stmt);
    return -1;
}

int sqlite_block_upsert(sqlite_db_t *db, int32_t devno, int64_t offset,
                        int32_t size, uint64_t hash, int ack) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_UPSERT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_upsert: prepare failed: %s", sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_bind_int(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)hash);
    sqlite3_bind_int(stmt, 5, ack);
    sqlite3_bind_text(stmt, 6, db->remote_id, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_upsert: step failed: %s", sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

int sqlite_block_mark_acked(sqlite_db_t *db, int32_t devno, int64_t offset) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_MARK_ACK, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_mark_acked: step failed: %s", sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

int sqlite_get_unacked(sqlite_db_t *db,
                       int32_t *devnos, int64_t *offsets,
                       int max_entries, const char *remote_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_GET_UNACKED, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_entries);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_entries) {
        devnos[count]  = sqlite3_column_int(stmt, 0);
        offsets[count] = sqlite3_column_int64(stmt, 1);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}
