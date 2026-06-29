# go2cloud 模块接口文档 v1.0

## 1. 共享层 — include/protocol.h

所有客户端和服务端模块的共享协议定义。

**关键常量**:
```c
BLOCK_SIZE              0x100000   // 1MB
SERVER_RESPONSE_SIZE    20         // sizeof(server_response_t)
SOCKET_POOL_TARGET      7          // 并发连接数
BACKPRESSURE_QUEUE_DEPTH 40       // 队列背压阈值
```

**关键类型**:
```c
server_response_t   // 20B 服务端响应
block_message_t     // 内存中的块消息表示
```

---

## 2. 日志模块 — log.h / log.c

**位置**: 客户端和服务端各有一份副本 (功能相同)。

### 接口

```c
void log_init(int level, const char *path);
void log_write(int level, const char *file, int line, const char *fmt, ...);
void log_close(void);
```

### 宏

```c
LOG_TRACE(fmt, ...)  // level 1
LOG_DEBUG(fmt, ...)  // level 2
LOG_INFO(fmt, ...)   // level 3
LOG_WARN(fmt, ...)   // level 4
LOG_ERROR(fmt, ...)  // level 5
```

### 线程安全

Windows: `CRITICAL_SECTION` / Linux: `pthread_mutex_t`。

---

## 3. 哈希模块 — client/hash.h / hash.c

### 接口

```c
uint64_t hash_block(const void *data, size_t len, uint64_t seed);
```

### 算法

基于 xxHash64 结构的自定义哈希, 使用非标准常数防碰撞。
- 短数据 (< 32B): `len + HASH_INIT_SHORT + seed`
- 长数据 (≥ 32B): 4 状态并行迭代 + fmix64 最终混合

---

## 4. MsgPack 编码模块 — client/msgpack.h / msgpack.c

### 接口

```c
void msgpack_writer_init(msgpack_writer_t *w);
void msgpack_writer_free(msgpack_writer_t *w);
void msgpack_writer_reset(msgpack_writer_t *w);
int  msgpack_encode_block(msgpack_writer_t *w,
                          int32_t devno, int64_t offset,
                          const uint8_t *data, uint32_t data_len);
```

### 内部结构

```c
typedef struct {
    uint8_t *buf;      // 动态扩容缓冲区 (初始 8KB)
    size_t   written;  // 已写入字节
    size_t   capacity; // 当前容量
} msgpack_writer_t;
```

### 编码输出

```
0x93 0x00 <devno int> 0x01 <offset int64> 0x02 <bin_header><data>
```

---

## 5. 有线协议模块 — client/wire.h / wire.c

### 接口

```c
int wire_send(socket_t fd,
              const uint8_t *msg_data, size_t msg_len,
              int zstd_level,
              uint8_t *scratch_buf, size_t scratch_cap);

int wire_send_control(socket_t fd,
                      const char *str, size_t len);
```

### 发送路径

```
MsgPack payload → Zstd compress → prepend "abc" → prepend 4B BE len → send()
Control:    4B BE len + raw string → send()
```

---

## 6. 发送队列模块 — client/queue.h / queue.c

### 接口

```c
int queue_init(send_queue_t *q);
void queue_destroy(send_queue_t *q);
int queue_push(send_queue_t *q, int32_t devno, int64_t offset, uint64_t hash,
               const uint8_t *payload, size_t payload_len);
int queue_pop(send_queue_t *q, queue_entry_t *out);
void queue_ack(send_queue_t *q, int32_t devno, int64_t offset);
int queue_count(send_queue_t *q);
int queue_get_pending(send_queue_t *q, int32_t *devnos, int64_t *offsets, int max);
int queue_should_backpressure(send_queue_t *q);
```

### 数据结构

```c
#define QUEUE_CAPACITY 256

typedef struct {
    int32_t  devno;
    int64_t  offset;
    uint64_t hash;
    uint8_t  payload[0x100000 + 256];
    size_t   payload_len;
    int      pending;   // 1=待发送, 0=空闲/已确认
} queue_entry_t;
```

### 返回值

- `queue_push`: 0=成功, 2=触发背压, -1=队列满
- `queue_pop`: 0=成功, -1=队列空

---

## 7. 连接池模块 — client/pool.h / pool.c

### 接口

```c
int pool_init(socket_pool_t *p, const char *server_ip, uint16_t server_port);
void pool_destroy(socket_pool_t *p);
pool_conn_t *pool_acquire(socket_pool_t *p);
void pool_release(socket_pool_t *p, pool_conn_t *c);
int pool_reap_stale(socket_pool_t *p);
int pool_recv_ack(pool_conn_t *c, server_response_t *resp);
int pool_count(socket_pool_t *p);
void pool_touch(pool_conn_t *c);
```

### 数据结构

```c
typedef struct {
    socket_t  fd;
    char      server_ip[64];
    uint16_t  server_port;
    int       in_use;
    time_t    last_active;
    uint64_t  blocks_sent;
    uint64_t  bytes_sent;
    uint8_t   send_scratch[0x100000 + 1024];
} pool_conn_t;
```

### 行为

- `pool_acquire()`: 优先返回空闲连接; 无空闲时尝试新建 (最多 SOCKET_POOL_TARGET 个)
- `pool_release()`: 标记连接为空闲 (不关闭)
- `pool_reap_stale()`: 关闭超过 12 分钟无活动的空闲连接
- `pool_recv_ack()`: 非阻塞接收 20B 响应, 0=无数据, 1=成功, -1=错误

---

## 8. 定时器模块 — client/timer.h / timer.c

### 接口

```c
void timer_init(timer_mgr_t *tm);
timer_event_t timer_check(timer_mgr_t *tm, uint64_t now_ms);
void timer_reset(timer_mgr_t *tm, timer_event_t event);
uint64_t timer_now_ms(void);
```

### 事件

| 事件 | 间隔 | 触发操作 |
|------|------|----------|
| TIMER_RETRANSMIT | 18s | 重传未 ACK 块 |
| TIMER_RECONNECT | 20s | 回收过期连接, 重建 |
| TIMER_ACTION | 10s | 输出进度统计 |

---

## 9. SQLite 模块 — client/sqlite.h / sqlite.c

### 接口

```c
sqlite_db_t *sqlite_open(const char *db_path);
void sqlite_close(sqlite_db_t *db);
int sqlite_block_lookup(sqlite_db_t *db, int32_t devno, int64_t offset,
                        uint64_t *out_hash);
int sqlite_block_acked(sqlite_db_t *db, int32_t devno, int64_t offset);
int sqlite_block_upsert(sqlite_db_t *db, int32_t devno, int64_t offset,
                        int32_t size, uint64_t hash, int ack);
int sqlite_block_mark_acked(sqlite_db_t *db, int32_t devno, int64_t offset);
int sqlite_get_unacked(sqlite_db_t *db, int32_t *devnos, int64_t *offsets,
                       int max_entries, const char *remote_id);
void sqlite_set_remote_id(sqlite_db_t *db, const char *remote_id);
```

### 表结构

```sql
CREATE TABLE T_BLOCK(
    devno     INTEGER,
    offset    INTEGER,
    size      INTEGER,
    hash      INTEGER,
    ack       INTEGER DEFAULT 0,
    remote_id TEXT
);
CREATE UNIQUE INDEX idx_block ON T_BLOCK(devno, offset);
```

### 返回约定

- `sqlite_block_lookup`: 0=找到, -1=不存在, -2=DB错误
- `sqlite_block_acked`: 1=已确认, 0=未确认, -1=不存在
- `sqlite_block_upsert`: 0=成功, -1=失败

---

## 10. 卷枚举模块 — client/volume.h / volume.c

### 接口

```c
int volume_enumerate(volume_list_t *list);
uint64_t volume_block_count(const volume_info_t *vol, uint32_t block_size);
```

### 数据结构

```c
typedef struct {
    int32_t  devno;
    char     path[256];
    uint64_t total_bytes;
    uint64_t block_count;
    int      writable;
} volume_info_t;

typedef struct {
    volume_info_t volumes[16];
    int           count;
} volume_list_t;
```

### 平台

- Windows: `CreateFile(\\.\PhysicalDriveN)` + `IOCTL_DISK_GET_DRIVE_GEOMETRY`
- Linux: 桩实现, 返回占位卷

---

## 11. 块 I/O 模块 — client/block_io.h / block_io.c

### 接口

```c
block_reader_t *block_reader_open(const char *path);
void block_reader_close(block_reader_t *r);
uint64_t block_reader_size(block_reader_t *r);
uint64_t block_reader_block_count(block_reader_t *r, uint32_t block_size);
int block_reader_read(block_reader_t *r, uint64_t offset,
                      uint8_t *buf, uint32_t buf_size,
                      uint32_t *bytes_read);
```

### 平台

- Windows: `CreateFile` + `SetFilePointerEx` + `ReadFile` + 扇区对齐
- Linux: `open` + `pread64` (+ `O_DIRECT` 尝试)

---

## 12. VSS 模块 — client/vss.h / vss.c (仅 Windows)

### 接口

```c
vss_context_t *vss_init(void);
void vss_cleanup(vss_context_t *ctx);
int vss_create_snapshots(vss_context_t *ctx,
                         const char *volumes[],
                         vss_snapshot_t *snapshots);
int vss_backup_complete(vss_context_t *ctx);
const char *vss_snapshot_device_path(const vss_snapshot_t *snap);
```

### VSS 生命周期 (12 步)

```
CreateVssBackupComponents → InitializeForBackup → SetBackupState
→ GatherWriterMetadata → StartSnapshotSet → AddToSnapshotSet
→ SetBackupState(BACKUP) → PrepareForBackup → DoSnapshotSet
→ GetSnapshotProperties → [读取数据] → BackupComplete
```

---

## 13. 服务端 — 会话管理 (session.h / session.c)

### 接口

```c
void session_mgr_init(void);
session_t *session_create(int fd, const char *remote_addr);
session_t *session_find(int fd);
void session_remove(int fd);
int session_count(void);
void session_foreach(int (*cb)(session_t *, void *), void *arg);
void session_touch(session_t *s);
```

### 数据结构

```c
typedef struct {
    int      fd;
    char     remote_addr[64];
    int      incremental;
    time_t   last_active;
    uint64_t blocks_received;
    uint8_t  recv_buf[4MB];   // TCP 粘包缓冲
    size_t   recv_len;
    uint32_t frame_len;
} session_t;
```

---

## 14. 服务端 — 协议解码器 (protocol_decoder.h / protocol_decoder.c)

### 接口

```c
int protocol_decode(session_t *session,
                    const uint8_t *buf, size_t len,
                    decoded_msg_t *out);
```

### 返回值

- `1`: 成功解码一条消息, `out` 有效
- `0`: 需要更多数据 (帧未完整)
- `-1`: 协议错误

### 解码流程

```
recv_buf 累积 → 读 4B BE 帧长 → 等待完整帧 →
检查 "abc" 魔数 → [YES: Zstd 解压 + MsgPack 解析]
                  → [NO:  控制帧检测]
```

### 输出结构

```c
typedef struct {
    decode_result_t type;    // DATA_BLOCK / CTL_INCREMENTAL / CTL_END
    int32_t  devno;
    int64_t  offset;
    uint8_t  data[1MB];
    uint32_t data_len;
} decoded_msg_t;
```

---

## 15. 服务端 — 块写入器 (block_writer.h / block_writer.c)

### 接口

```c
void block_writer_init(void);
int block_writer_register(int devno, const char *path);
int block_writer_open_all(void);
int block_writer_write(int32_t devno, int64_t offset,
                       const uint8_t *data, uint32_t len);
int block_writer_fsync_all(void);
void block_writer_close_all(void);
uint64_t block_writer_total_bytes(void);
uint64_t block_writer_total_blocks(void);
```

### 写入流程

```
注册 devno→path → open() → lseek64(offset) → write() → [return 0]
fsync: 遍历所有打开的 fd → fsync()
```

---

## 16. 服务端 — ACK 生成器 (ack.h / ack.c)

### 接口

```c
int ack_send_block(int fd, int32_t devno, int32_t size, int64_t offset);
int ack_send_done(int fd);
int ack_send_binlog(int fd, int32_t devno, int32_t size, int64_t offset);
```

### 响应格式

20 字节 `server_response_t` 结构体, 直接通过 TCP 发送原始字节。
