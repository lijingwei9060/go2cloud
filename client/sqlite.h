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
 * 获取所有未确认块的 devno/offset/hash/last_sent 列表。
 * 用于增量同步: 从 live disk 重读并 hash 对比, 仅重发变化的块。
 *
 * hashes:          [out] 块哈希值数组 (可为 NULL)
 * last_sent_times: [out] 上次发送时间数组 (可为 NULL)
 * 返回条目数, 错误返回 -1。
 */
int sqlite_get_unacked_with_hash(sqlite_db_t *db,
                                  int32_t *devnos, int64_t *offsets,
                                  uint64_t *hashes, int64_t *last_sent_times,
                                  int max_entries, const char *remote_id);

/*
 * 更新块的 last_sent 时间戳。
 * timestamp_ms: 当前时间 (毫秒)
 */
int sqlite_update_last_sent(sqlite_db_t *db, int32_t devno, int64_t offset,
                             int64_t timestamp_ms);

/*
 * 统计未确认块的总数。
 */
int sqlite_count_unacked(sqlite_db_t *db, const char *remote_id);

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

/*
 * 块信息结构体 — 用于查询 T_BLOCK 单行所有字段。
 */
typedef struct {
    int32_t  devno;
    int64_t  offset;
    int32_t  size;
    uint64_t hash;
    int      ack;
    int64_t  last_sent;
    int      version;
    char     remote_id[256];
} block_info_t;

/* 版本/轮次信息 */
typedef struct {
    int      version;
    int64_t  start_time;
    int64_t  end_time;
    int      scanned_count;
    int      changed_count;
} version_info_t;

/*
 * 查询单个块的所有信息。
 * 返回: 0=找到, -1=未找到, -2=数据库错误
 */
int sqlite_get_block_info(sqlite_db_t *db, int32_t devno, int64_t offset,
                          block_info_t *info);

/*
 * 列出所有块 (可按 devno 过滤)。
 * devno < 0 表示不过滤。
 * infos: 输出数组, max_count: 最大条目数
 * 返回条目数, 错误返回 -1。
 */
int sqlite_list_blocks(sqlite_db_t *db, int32_t devno,
                       block_info_t *infos, int max_count);

/*
 * 统计块数 (可按 devno 和 ack 过滤)。
 * devno < 0 表示不过滤, ack < 0 表示不过滤。
 */
int sqlite_count_blocks(sqlite_db_t *db, int32_t devno, int ack);

/*
 * 重置所有已确认块的 ACK 状态 (ack=1 → ack=0)。
 * 在增量同步每轮开始时调用, 触发从 live disk 重读验证。
 *
 * remote_id: 仅重置匹配的 remote_id 行
 * 返回重置的行数, 错误返回 -1。
 */
int sqlite_reset_acked(sqlite_db_t *db, const char *remote_id);

/* ================================================================
 * 版本/轮次管理 (T_VERSION)
 * ================================================================ */

/*
 * 获取下一个版本号 (自动递增)。
 * 查询 T_VERSION 中的 MAX(version)+1, 若表为空则返回 1。
 * 按 remote_id 分组, 不同服务端各自计数。
 */
int sqlite_get_next_version(sqlite_db_t *db, const char *remote_id);

/*
 * 记录一轮同步开始。
 * 在 T_VERSION 中插入一行 (version, remote_id, start_time)。
 */
int sqlite_version_start(sqlite_db_t *db, int version,
                         const char *remote_id, int64_t start_time_ms);

/*
 * 记录一轮同步结束。
 * 更新 T_VERSION 的 end_time, 并累加 scanned_count 和 changed_count。
 */
int sqlite_version_end(sqlite_db_t *db, int version,
                       const char *remote_id, int64_t end_time_ms,
                       int scanned_delta, int changed_delta);

/*
 * Upsert 块记录 (带版本号)。
 * 与 sqlite_block_upsert 相同, 但显式指定 version 列。
 */
int sqlite_block_upsert_v(sqlite_db_t *db, int32_t devno, int64_t offset,
                          int32_t size, uint64_t hash, int ack, int version);

/*
 * 标记块为已在本轮验证 (设置 version + ack=1)。
 * 用于增量同步中 hash 未变化的块。
 */
int sqlite_block_mark_verified(sqlite_db_t *db, int32_t devno,
                               int64_t offset, int version);

/*
 * 统计未确认块数 (按版本过滤)。
 * 仅统计 version < before_version 且 ack=0 的块。
 */
int sqlite_count_unacked_v(sqlite_db_t *db, const char *remote_id,
                           int before_version);

/*
 * 查询版本历史。
 * 返回 T_VERSION 表中指定 remote_id 的所有轮次信息。
 */
int sqlite_get_version_history(sqlite_db_t *db, const char *remote_id,
                               version_info_t *infos, int max_count);

/*
 * 查询单个版本信息。
 */
int sqlite_get_version_info(sqlite_db_t *db, int version,
                            const char *remote_id, version_info_t *info);

/*
 * 统计指定版本的块数。
 */
int sqlite_count_blocks_by_version(sqlite_db_t *db, int version,
                                   const char *remote_id);

#endif /* CLIENT_SQLITE_H */
