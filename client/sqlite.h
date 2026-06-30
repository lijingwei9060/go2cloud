/*
 * sqlite.h — SQLite 块跟踪模块
 *
 * 管理 T_BLOCK 表, 用于块去重检测, 实现增量迁移。
 *
 * 表结构:
 *   CREATE TABLE T_BLOCK(
 *     devno     INTEGER,   -- 磁盘编号
 *     offset    INTEGER,   -- 块偏移 (字节)
 *     size      INTEGER,   -- 块大小 (1MB)
 *     hash      INTEGER,   -- 块内容 64-bit 哈希
 *     ack       INTEGER,   -- 0=未确认, 1=已确认
 *     remote_id TEXT        -- 远程主机标识
 *   );
 *   CREATE UNIQUE INDEX idx_block ON T_BLOCK(devno, offset);
 *
 * 增量逻辑:
 *   1. 读取块 → 计算 hash
 *   2. 查询 T_BLOCK (devno, offset)
 *   3. 如果 hash 相同 → 跳过 (内容未变)
 *   4. 如果 hash 不同 → 发送新块, 更新 hash
 */

#ifndef CLIENT_SQLITE_H
#define CLIENT_SQLITE_H

#include <stdint.h>

/* 不透明数据库句柄 (避免头文件依赖 sqlite3.h) */
typedef struct sqlite_db sqlite_db_t;

/*
 * 打开 (或创建) 跟踪数据库。
 *
 * db_path: SQLite 数据库文件路径 (通常为 tracker.db)
 *
 * 返回不透明句柄, 失败返回 NULL。
 */
sqlite_db_t *sqlite_open(const char *db_path);

/* 关闭数据库 */
void sqlite_close(sqlite_db_t *db);

/*
 * 查询块的哈希值。
 *
 * 返回:
 *    0  — 块存在, *out_hash 为存储的哈希值
 *   -1  — 块不存在
 *   -2  — 数据库错误
 */
int sqlite_block_lookup(sqlite_db_t *db, int32_t devno, int64_t offset,
                        uint64_t *out_hash);

/*
 * 检查块是否已确认 (ack=1)。
 *
 * 返回:
 *    1  — 已确认
 *    0  — 未确认
 *   -1  — 块不存在
 */
int sqlite_block_acked(sqlite_db_t *db, int32_t devno, int64_t offset);

/*
 * 更新或插入块的跟踪信息。
 *
 * 使用 INSERT OR REPLACE (通过唯一索引 devno+offset 实现 upsert)。
 */
int sqlite_block_upsert(sqlite_db_t *db, int32_t devno, int64_t offset,
                        int32_t size, uint64_t hash, int ack);

/*
 * 标记块为已确认 (ack=1)。
 * 在收到服务端 ACK 后调用。
 */
int sqlite_block_mark_acked(sqlite_db_t *db, int32_t devno, int64_t offset);

/*
 * 获取所有未确认块的 devno/offset 列表。
 * 用于重传扫描。
 *
 * 返回条目数, 错误返回 -1。
 */
int sqlite_get_unacked(sqlite_db_t *db,
                       int32_t *devnos, int64_t *offsets,
                       int max_entries, const char *remote_id);

/*
 * 查询已确认块的总字节数 (ack=1)。
 * 用于跨进程进度查询 (sentbytes 子命令)。
 */
int64_t sqlite_total_acked_bytes(sqlite_db_t *db);

/*
 * 删除 T_BLOCK 表中所有记录。
 * 返回 0 成功, -1 失败。
 */
int sqlite_clear_all_blocks(sqlite_db_t *db);

/*
 * 设置当前会话的 remote_id (用于多会话区分)。
 */
void sqlite_set_remote_id(sqlite_db_t *db, const char *remote_id);

#endif /* CLIENT_SQLITE_H */
