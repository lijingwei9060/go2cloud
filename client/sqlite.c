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
    "  last_sent INTEGER DEFAULT 0," \
    "  version   INTEGER DEFAULT 0," \
    "  remote_id TEXT" \
    ");" \
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_block ON T_BLOCK(devno, offset);" \
    "CREATE INDEX IF NOT EXISTS idx_ack ON T_BLOCK(ack);" \
    "CREATE INDEX IF NOT EXISTS idx_version ON T_BLOCK(remote_id, version);"

#define SQL_LOOKUP \
    "SELECT hash FROM T_BLOCK WHERE devno=? AND offset=?"

#define SQL_CHECK_ACK \
    "SELECT ack FROM T_BLOCK WHERE devno=? AND offset=?"

#define SQL_UPSERT \
    "INSERT OR REPLACE INTO T_BLOCK(devno, offset, size, hash, ack, last_sent, remote_id) " \
    "VALUES(?, ?, ?, ?, ?, ?, ?)"

#define SQL_MARK_ACK \
    "UPDATE T_BLOCK SET ack=1 WHERE devno=? AND offset=?"

#define SQL_GET_UNACKED \
    "SELECT devno, offset FROM T_BLOCK WHERE ack=0 AND remote_id=? " \
    "ORDER BY last_sent, offset LIMIT ?"

#define SQL_GET_UNACKED_WITH_HASH \
    "SELECT devno, offset, hash, last_sent FROM T_BLOCK " \
    "WHERE ack=0 AND remote_id=? ORDER BY last_sent, offset LIMIT ?"

#define SQL_UPDATE_LAST_SENT \
    "UPDATE T_BLOCK SET last_sent=? WHERE devno=? AND offset=?"

#define SQL_COUNT_UNACKED \
    "SELECT COUNT(*) FROM T_BLOCK WHERE ack=0 AND remote_id=?"

#define SQL_RESET_ACKED \
    "UPDATE T_BLOCK SET ack=0 WHERE ack=1 AND remote_id=?"

/* 版本化 upsert */
#define SQL_UPSERT_V \
    "INSERT OR REPLACE INTO T_BLOCK(devno, offset, size, hash, ack, last_sent, version, remote_id) " \
    "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"

/* 标记块在指定版本中已验证 */
#define SQL_MARK_VERIFIED \
    "UPDATE T_BLOCK SET version=?, ack=1 WHERE devno=? AND offset=?"

/* T_VERSION 操作 */
#define SQL_CREATE_T_VERSION \
    "CREATE TABLE IF NOT EXISTS T_VERSION(" \
    "  id            INTEGER PRIMARY KEY AUTOINCREMENT," \
    "  version       INTEGER NOT NULL," \
    "  remote_id     TEXT," \
    "  start_time    INTEGER," \
    "  end_time      INTEGER," \
    "  scanned_count INTEGER DEFAULT 0," \
    "  changed_count INTEGER DEFAULT 0" \
    ");" \
    "CREATE INDEX IF NOT EXISTS idx_version_remote ON T_VERSION(remote_id, version);"

#define SQL_NEXT_VERSION \
    "SELECT COALESCE(MAX(version), 0) + 1 FROM T_VERSION WHERE remote_id=?"

#define SQL_INSERT_VERSION \
    "INSERT INTO T_VERSION(version, remote_id, start_time) VALUES(?, ?, ?)"

#define SQL_UPDATE_VERSION_END \
    "UPDATE T_VERSION SET end_time=?, scanned_count=scanned_count+?, " \
    "changed_count=changed_count+? WHERE version=? AND remote_id=?"

#define SQL_GET_VERSION_HISTORY \
    "SELECT version, start_time, end_time, scanned_count, changed_count " \
    "FROM T_VERSION WHERE remote_id=? ORDER BY version"

#define SQL_GET_VERSION_HISTORY_ALL \
    "SELECT version, start_time, end_time, scanned_count, changed_count " \
    "FROM T_VERSION ORDER BY version"

#define SQL_GET_VERSION_INFO \
    "SELECT version, start_time, end_time, scanned_count, changed_count " \
    "FROM T_VERSION WHERE version=? AND remote_id=?"

#define SQL_COUNT_UNACKED_V \
    "SELECT COUNT(*) FROM T_BLOCK WHERE ack=0 AND remote_id=? AND version < ?"

#define SQL_COUNT_BLOCKS_BY_VERSION \
    "SELECT COUNT(*) FROM T_BLOCK WHERE version=? AND remote_id=?"

#define SQL_COUNT_BLOCKS_BY_VERSION_ALL \
    "SELECT COUNT(*) FROM T_BLOCK WHERE version=?"

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

    /* 迁移: 为已有数据库添加 version 列 */
    {
        sqlite3_stmt *stmt = NULL;
        int has_version = 0;
        rc = sqlite3_prepare_v2(handle, "PRAGMA table_info(T_BLOCK)",
                                -1, &stmt, NULL);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *col = (const char *)sqlite3_column_text(stmt, 1);
                if (col && strcmp(col, "version") == 0) {
                    has_version = 1;
                    break;
                }
            }
            sqlite3_finalize(stmt);
        }
        if (!has_version) {
            rc = sqlite3_exec(handle,
                "ALTER TABLE T_BLOCK ADD COLUMN version INTEGER DEFAULT 0",
                NULL, NULL, &err);
            if (rc != SQLITE_OK) {
                LOG_WARN("sqlite: ALTER TABLE ADD version failed: %s", err);
                sqlite3_free(err);
            } else {
                LOG_INFO("sqlite: added version column to existing T_BLOCK");
            }
        }
    }

    /* 创建 T_VERSION 表 */
    rc = sqlite3_exec(handle, SQL_CREATE_T_VERSION, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite: CREATE T_VERSION failed: %s", err);
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
    return sqlite_block_upsert_v(db, devno, offset, size, hash, ack, 0);
}

int sqlite_block_upsert_v(sqlite_db_t *db, int32_t devno, int64_t offset,
                          int32_t size, uint64_t hash, int ack, int version) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_UPSERT_V, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_upsert_v: prepare failed: %s", sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);
    sqlite3_bind_int(stmt, 3, size);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)hash);
    sqlite3_bind_int(stmt, 5, ack);
    sqlite3_bind_int64(stmt, 6, 0);  /* last_sent */
    sqlite3_bind_int(stmt, 7, version);
    sqlite3_bind_text(stmt, 8, db->remote_id, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_upsert_v: step failed: %s", sqlite3_errmsg(db->handle));
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

int sqlite_get_unacked_with_hash(sqlite_db_t *db,
                                  int32_t *devnos, int64_t *offsets,
                                  uint64_t *hashes, int64_t *last_sent_times,
                                  int max_entries, const char *remote_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_GET_UNACKED_WITH_HASH, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_entries);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_entries) {
        devnos[count]          = sqlite3_column_int(stmt, 0);
        offsets[count]         = sqlite3_column_int64(stmt, 1);
        if (hashes)            hashes[count] = (uint64_t)sqlite3_column_int64(stmt, 2);
        if (last_sent_times)   last_sent_times[count] = sqlite3_column_int64(stmt, 3);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int sqlite_update_last_sent(sqlite_db_t *db, int32_t devno, int64_t offset,
                             int64_t timestamp_ms) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_UPDATE_LAST_SENT, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int64(stmt, 1, timestamp_ms);
    sqlite3_bind_int(stmt, 2, devno);
    sqlite3_bind_int64(stmt, 3, offset);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int sqlite_get_block_info(sqlite_db_t *db, int32_t devno, int64_t offset,
                          block_info_t *info) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT devno, offset, size, hash, ack, last_sent, "
                      "IFNULL(version, 0), IFNULL(remote_id, '') FROM T_BLOCK "
                      "WHERE devno=? AND offset=?";
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -2;

    sqlite3_bind_int(stmt, 1, devno);
    sqlite3_bind_int64(stmt, 2, offset);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(info, 0, sizeof(*info));
        info->devno     = sqlite3_column_int(stmt, 0);
        info->offset    = sqlite3_column_int64(stmt, 1);
        info->size      = sqlite3_column_int(stmt, 2);
        info->hash      = (uint64_t)sqlite3_column_int64(stmt, 3);
        info->ack       = sqlite3_column_int(stmt, 4);
        info->last_sent = sqlite3_column_int64(stmt, 5);
        info->version   = sqlite3_column_int(stmt, 6);
        const unsigned char *rid = sqlite3_column_text(stmt, 7);
        if (rid) {
            strncpy(info->remote_id, (const char *)rid,
                    sizeof(info->remote_id) - 1);
        }
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? -1 : -2;
}

int sqlite_list_blocks(sqlite_db_t *db, int32_t devno,
                       block_info_t *infos, int max_count) {
    sqlite3_stmt *stmt = NULL;
    const char *sql;
    if (devno >= 0) {
        sql = "SELECT devno, offset, size, hash, ack, last_sent, "
              "IFNULL(version, 0), IFNULL(remote_id, '') FROM T_BLOCK "
              "WHERE devno=? ORDER BY offset LIMIT ?";
    } else {
        sql = "SELECT devno, offset, size, hash, ack, last_sent, "
              "IFNULL(version, 0), IFNULL(remote_id, '') FROM T_BLOCK "
              "ORDER BY devno, offset LIMIT ?";
    }
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    int idx = 1;
    if (devno >= 0) {
        sqlite3_bind_int(stmt, idx++, devno);
    }
    sqlite3_bind_int(stmt, idx++, max_count);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        block_info_t *info = &infos[count];
        memset(info, 0, sizeof(*info));
        info->devno     = sqlite3_column_int(stmt, 0);
        info->offset    = sqlite3_column_int64(stmt, 1);
        info->size      = sqlite3_column_int(stmt, 2);
        info->hash      = (uint64_t)sqlite3_column_int64(stmt, 3);
        info->ack       = sqlite3_column_int(stmt, 4);
        info->last_sent = sqlite3_column_int64(stmt, 5);
        info->version   = sqlite3_column_int(stmt, 6);
        const unsigned char *rid = sqlite3_column_text(stmt, 7);
        if (rid) {
            strncpy(info->remote_id, (const char *)rid,
                    sizeof(info->remote_id) - 1);
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int sqlite_count_blocks(sqlite_db_t *db, int32_t devno, int ack) {
    sqlite3_stmt *stmt = NULL;
    char sql[256];
    int idx = 1;

    strncpy(sql, "SELECT COUNT(*) FROM T_BLOCK WHERE 1=1", sizeof(sql) - 1);
    sql[sizeof(sql) - 1] = '\0';

    if (devno >= 0) {
        strncat(sql, " AND devno=?", sizeof(sql) - strlen(sql) - 1);
    }
    if (ack >= 0) {
        strncat(sql, " AND ack=?", sizeof(sql) - strlen(sql) - 1);
    }

    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    if (devno >= 0) sqlite3_bind_int(stmt, idx++, devno);
    if (ack >= 0) sqlite3_bind_int(stmt, idx++, ack);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int sqlite_reset_acked(sqlite_db_t *db, const char *remote_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_RESET_ACKED, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_reset_acked: prepare failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db->handle);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_reset_acked: step failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }

    LOG_INFO("sqlite: reset %d blocks (ack=1 → ack=0)", changed);
    return changed;
}

int sqlite_count_unacked(sqlite_db_t *db, const char *remote_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_COUNT_UNACKED, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ================================================================
 * 版本/轮次管理
 * ================================================================ */

int sqlite_get_next_version(sqlite_db_t *db, const char *remote_id) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_NEXT_VERSION, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);

    int version = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

int sqlite_version_start(sqlite_db_t *db, int version,
                         const char *remote_id, int64_t start_time_ms) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_INSERT_VERSION, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_version_start: prepare failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, version);
    sqlite3_bind_text(stmt, 2, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, start_time_ms);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_version_start: step failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }
    LOG_INFO("sqlite: version %d started", version);
    return 0;
}

int sqlite_version_end(sqlite_db_t *db, int version,
                       const char *remote_id, int64_t end_time_ms,
                       int scanned_delta, int changed_delta) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_UPDATE_VERSION_END,
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_version_end: prepare failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, end_time_ms);
    sqlite3_bind_int(stmt, 2, scanned_delta);
    sqlite3_bind_int(stmt, 3, changed_delta);
    sqlite3_bind_int(stmt, 4, version);
    sqlite3_bind_text(stmt, 5, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_version_end: step failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }
    LOG_INFO("sqlite: version %d ended (scanned=%d changed=%d)",
             version, scanned_delta, changed_delta);
    return 0;
}

int sqlite_block_mark_verified(sqlite_db_t *db, int32_t devno,
                               int64_t offset, int version) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_MARK_VERIFIED, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        LOG_ERROR("sqlite_mark_verified: prepare failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, version);
    sqlite3_bind_int(stmt, 2, devno);
    sqlite3_bind_int64(stmt, 3, offset);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        LOG_ERROR("sqlite_mark_verified: step failed: %s",
                  sqlite3_errmsg(db->handle));
        return -1;
    }
    return 0;
}

int sqlite_count_unacked_v(sqlite_db_t *db, const char *remote_id,
                           int before_version) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_COUNT_UNACKED_V,
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, before_version);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

int sqlite_get_version_history(sqlite_db_t *db, const char *remote_id,
                               version_info_t *infos, int max_count) {
    const char *rid = remote_id ? remote_id : db->remote_id;
    int has_rid = (rid && rid[0]);

    sqlite3_stmt *stmt = NULL;
    const char *sql = has_rid ? SQL_GET_VERSION_HISTORY
                              : SQL_GET_VERSION_HISTORY_ALL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    if (has_rid)
        sqlite3_bind_text(stmt, 1, rid, -1, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_count) {
        version_info_t *v = &infos[count];
        v->version       = sqlite3_column_int(stmt, 0);
        v->start_time    = sqlite3_column_int64(stmt, 1);
        v->end_time      = sqlite3_column_int64(stmt, 2);
        v->scanned_count = sqlite3_column_int(stmt, 3);
        v->changed_count = sqlite3_column_int(stmt, 4);
        count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

int sqlite_get_version_info(sqlite_db_t *db, int version,
                            const char *remote_id, version_info_t *info) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->handle, SQL_GET_VERSION_INFO,
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -2;

    sqlite3_bind_int(stmt, 1, version);
    sqlite3_bind_text(stmt, 2, remote_id ? remote_id : db->remote_id,
                      -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        memset(info, 0, sizeof(*info));
        info->version       = sqlite3_column_int(stmt, 0);
        info->start_time    = sqlite3_column_int64(stmt, 1);
        info->end_time      = sqlite3_column_int64(stmt, 2);
        info->scanned_count = sqlite3_column_int(stmt, 3);
        info->changed_count = sqlite3_column_int(stmt, 4);
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? -1 : -2;
}

int sqlite_count_blocks_by_version(sqlite_db_t *db, int version,
                                   const char *remote_id) {
    const char *rid = remote_id ? remote_id : db->remote_id;
    int has_rid = (rid && rid[0]);

    sqlite3_stmt *stmt = NULL;
    const char *sql = has_rid ? SQL_COUNT_BLOCKS_BY_VERSION
                              : SQL_COUNT_BLOCKS_BY_VERSION_ALL;
    int rc = sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_int(stmt, 1, version);
    if (has_rid)
        sqlite3_bind_text(stmt, 2, rid, -1, SQLITE_STATIC);

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}
