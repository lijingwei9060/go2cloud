# go2cloud 有线协议规范 v1.0

## 1. 概述

go2cloud 协议用于客户端(Transfer)向服务端(Receiver)传输块级磁盘数据。
协议栈包含 4 层, 自底向上: TCP 分帧 → 帧同步 → Zstd 压缩 → MsgPack 消息。

**客户端** 读取本地磁盘块, 编码后发送。
**服务端** 接收、解码后将块写入目标磁盘, 返回 ACK 确认。

### 1.1 设计目标

- 跨平台: Windows / Linux 双端支持
- 压缩传输: Zstd 级别 1～7, 减少网络带宽
- 增量支持: 通过块哈希去重, 跳过未变化块
- 可靠性: ACK 确认 + 重传机制

### 1.2 术语

| 术语 | 说明 |
|------|------|
| 帧 (Frame) | 一条完整的 TCP 消息, 由 4B BE 长度前缀 + 载荷组成 |
| 魔数 (Magic) | 3 字节 "abc" (0x61 0x62 0x63), 标记数据帧的开始 |
| 块 (Block) | 1MB (1048576 字节) 固定大小的磁盘数据单元 |
| 控制消息 | 不经 MsgPack/Zstd 编码的原始字符串消息 |
| devno | 磁盘编号 (int32), 键 0 表示 |
| offset | 块在磁盘上的字节偏移 (int64), 键 1 表示 |

---

## 2. 协议栈全景

```
+--------------------------------------+
| 第 4 层 — MsgPack 消息               |
| fixmap(3) { 0:devno, 1:offset, 2:bin } |
+--------------------------------------+
| 第 3 层 — Zstd 压缩                  |
| 对 MsgPack 编码后的字节流压缩        |
+--------------------------------------+
| 第 2 层 — 帧同步                     |
| "abc" (3B) + 压缩后的载荷            |
+--------------------------------------+
| 第 1 层 — TCP 分帧                   |
| 4B BE length + 帧数据                |
+--------------------------------------+
```

---

## 3. 第 1 层 — TCP 分帧

### 3.1 格式

```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    帧长度 (大端 uint32)                       |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
|                    帧数据 (帧长度字节)                        |
|                    ...                                        |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
```

### 3.2 C 语言定义

```c
#define TCP_FRAME_HEADER_SIZE  4

/* 发送: 4B BE 长度 + 载荷 */
/* 接收: 读 4B → ntohl → 读 N 字节 → 完整帧 */
```

### 3.3 说明

- 帧长度 **不含** 4 字节头部自身, 仅表示后续帧数据的字节数
- 帧长度使用大端序 (网络字节序)
- 服务端使用 `recv_buf[4MB]` 缓冲, 支持 TCP 粘包/拆包重组

---

## 4. 第 2 层 — 帧同步

### 4.1 格式

```
 0               1               2
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  'a'   |  'b'   |  'c'   | 载荷... |
| 0x61   | 0x62   | 0x63   |        |
+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
```

### 4.2 C 语言定义

```c
#define FRAME_MAGIC        "abc"
#define FRAME_MAGIC_LEN    3
#define FRAME_MAGIC_B0     0x61
#define FRAME_MAGIC_B1     0x62
#define FRAME_MAGIC_B2     0x63
```

### 4.3 区分数据帧和控制帧

- **数据帧**: 帧数据以 "abc" 魔数开头 → 后续为 Zstd 压缩的 MsgPack 消息
- **控制帧**: 帧数据不以 "abc" 开头 → 载荷为原始 UTF-8 字符串 (如 "ctlIncremental")

---

## 5. 第 3 层 — Zstd 压缩

### 5.1 参数

| 参数 | 值 |
|------|-----|
| 压缩级别 | 1 (最快) ～ 7 (最高压缩) |
| 默认级别 | 7 (全量迁移) / 1 (增量尾随) |
| 目标缓冲区大小 | `ZSTD_COMPRESS_BOUND(src_len)` |

### 5.2 C 语言定义

```c
#define ZSTD_COMPRESS_LEVEL_MIN  1
#define ZSTD_COMPRESS_LEVEL_MAX  7

size_t compressed_len = ZSTD_compress(dst, dst_cap, src, src_len, level);
```

### 5.3 说明

- 仅对 **第 4 层 MsgPack 编码后的数据** 进行压缩
- 压缩发生在 "abc" 魔数之后
- 控制消息不经 Zstd 压缩

---

## 6. 第 4 层 — MsgPack 消息

### 6.1 块消息格式 (客户端 → 服务端)

```
 0x93  — fixmap(3) [1 字节]
 0x00  — key=0 (devno) [1 字节]
 <devno as int>      [1/2/3/5 字节, 自选最短编码]
 0x01  — key=1 (offset) [1 字节]
 <offset as int64>   [9 字节]
 0x02  — key=2 (data) [1 字节]
 <bin_header><data>  [2+len / 3+len / 5+len 字节]
```

### 6.2 C 语言定义

```c
#define MSGPACK_FIXMAP_3     0x93
#define MSGPACK_BIN8         0xc4
#define MSGPACK_BIN16        0xc5
#define MSGPACK_BIN32        0xc6
#define MSGPACK_INT8         0xd0
#define MSGPACK_INT16        0xd1
#define MSGPACK_INT32        0xd2
#define MSGPACK_INT64        0xd3

#define MSGPACK_KEY_DEVNO    0
#define MSGPACK_KEY_OFFSET   1
#define MSGPACK_KEY_DATA     2
```

### 6.3 devno 编码 (int32)

| 范围 | 编码 | 字节数 |
|------|------|--------|
| 0..127 | fixint (0xxxxxxx) | 1 |
| -32..-1 | fixint (111xxxxx) | 1 |
| -128..127 (超出 fixint) | int8 (0xd0 + 1B) | 2 |
| -32768..32767 (超出 int8) | int16 (0xd1 + 2B BE) | 3 |
| 其他 | int32 (0xd2 + 4B BE) | 5 |

### 6.4 offset 编码 (int64)

始终使用 `int64` 格式 (0xd3 + 8B BE), 共 9 字节。
偏移值可能超过 int32 范围 (2GB 以上磁盘)。

### 6.5 data 编码 (bin)

| 数据长度 | 编码 | 总字节数 |
|----------|------|----------|
| < 256 | bin8 (0xc4 + 1B len + data) | 2 + len |
| < 65536 | bin16 (0xc5 + 2B len BE + data) | 3 + len |
| >= 65536 | bin32 (0xc6 + 4B len BE + data) | 5 + len |

标准 1MB 块使用 **bin32** 路径。

---

## 7. 控制消息

### 7.1 ctlIncremental

通知服务端进入增量模式。后续块消息将包含哈希去重信息。

```
4B BE len(=15) + "ctlIncremental"
```

### 7.2 ctlEndIncremental

通知服务端增量轮次结束。服务端应在 fsync 后返回 SERVER_DONE。

```
4B BE len(=17) + "ctlEndIncremental"
```

### 7.3 C 语言常量

```c
#define CTL_INCREMENTAL         "ctlIncremental"
#define CTL_INCREMENTAL_LEN     15
#define CTL_END_INCREMENTAL     "ctlEndIncremental"
#define CTL_END_INCREMENTAL_LEN 17
```

---

## 8. 服务端响应

### 8.1 格式 — server_response_t (20 字节)

```c
#pragma pack(push, 1)
typedef struct {
    int32_t  type;      // 响应类型
    int32_t  devno;     // 磁盘编号 (回显)
    int32_t  size;      // 块大小 (回显)
    int64_t  offset;    // 块偏移 (回显)
} server_response_t;
#pragma pack(pop)

#define SERVER_RESPONSE_SIZE  20
```

### 8.2 响应类型

| 类型 | 值 | 说明 |
|------|-----|------|
| RESPONSE_ACK | 0 | 块已成功写入并 fsync |
| RESPONSE_ACK_ALT | 4 | 块确认变体 (同 ACK) |
| RESPONSE_BINLOG | 1 | binlog 超过 10GB 上限告警 |
| RESPONSE_SERVER_DONE | 2 | 增量轮次完成 (所有盘已 fsync) |

### 8.3 传输方式

- 响应 **不经任何编码**, 直接以 20 字节原始数据通过 TCP 发送
- 客户端 recv(20) 读取, 按 devno+offset 匹配对应块

---

## 9. 增量协议

### 9.1 原理: 哈希去重

```
客户端:                         服务端:
  │                               │
  ├─ ctlIncremental ─────────────>│  进入增量模式
  │                               │
  ├─ 读取块 @ offset              │
  ├─ 计算 hash = hash_block()     │
  ├─ SQLite: SELECT hash          │
  │    WHERE devno=X AND          │
  │          offset=Y             │
  │                               │
  ├─ if hash == stored_hash:      │
  │    SKIP (内容未变)             │
  │  else:                        │
  │    SEND block ───────────────>│  写入块
  │    UPDATE T_BLOCK             │      │
  │                               │<──── ACK ─┤
  │    mark ack=1                 │
  │                               │
  ├─ ctlEndIncremental ──────────>│  fsync 所有磁盘
  │                               │<─── SERVER_DONE ─┤
```

### 9.2 SQLite 表结构

```sql
CREATE TABLE T_BLOCK(
    devno     INTEGER,
    offset    INTEGER,
    size      INTEGER,
    hash      INTEGER,      -- 块内容的 64-bit 哈希
    ack       INTEGER DEFAULT 0,  -- 0=未确认, 1=已确认
    remote_id TEXT
);
CREATE UNIQUE INDEX idx_block ON T_BLOCK(devno, offset);
```

---

## 10. 哈希算法

### 10.1 算法: custom_hash (基于 xxHash64)

```c
uint64_t hash_block(const void *data, size_t len, uint64_t seed);
```

### 10.2 常数

| 常数 | 值 | 用途 |
|------|-----|------|
| HASH_MUL | 0xc2b4d1b3d54d51c2 | 乘法混合 |
| HASH_ROT | 0x9e3779b586c04815 | 旋转混合 |
| HASH_INIT_SHORT | 0x27d4eb2f165667c5 | 短数据初始值 |
| HASH_INIT_A | 0x60ea27eeadc0b5d6 | 状态 A 初始化 |
| HASH_INIT_B | 0x61c8864e7a143579 | 状态 B 初始化 |
| HASH_INIT_C | 0xc2b2ae3d27d4eb4f | 状态 C 初始化 |
| HASH_XOR_CONST | 0x85ebca77c2b2ae63 | 最终 XOR |

### 10.3 算法结构

```
1. 短数据 (< 32 字节): len + HASH_INIT_SHORT + seed
2. 长数据 (≥ 32 字节):
   a. 4 状态并行处理 (hash, state_b, state_c, state_d)
   b. 每次处理 32 字节 (4 × uint64_t)
   c. 每通道: hash += input * MUL; hash = rotl31(hash) * ROT
   d. 最终混合: 4 轮 fmix64 → XOR → MUL → ADD
   e. 混入长度: result += len
```

---

## 11. 连接管理

### 11.1 参数

| 参数 | 值 | 说明 |
|------|-----|------|
| SOCKET_POOL_TARGET | 7 | 目标并发连接数 |
| STALE_SOCKET_TIMEOUT_SEC | 720 | 12 分钟无活动 → 关闭 |
| RETRANSMIT_MIN_INTERVAL_SEC | 121 | 最小重传间隔 |
| MAX_CONNECT_RETRIES | 6 | 初始连接最大重试 |

### 11.2 背压控制

| 参数 | 值 | 说明 |
|------|-----|------|
| BACKPRESSURE_QUEUE_DEPTH | 40 | 队列深度超过此值 → 暂停读取 |
| BACKPRESSURE_MEMORY_MB | 45 | 内存使用超过此值 → 暂停读取 |
| BACKPRESSURE_SLEEP_MS | 50 | 背压等待间隔 |

### 11.3 定时器

| 定时器 | 间隔 | 操作 |
|--------|------|------|
| RETRANSMIT | 18 秒 | 扫描未 ACK 块, 重传 |
| RECONNECT | 20 秒 | 关闭过期连接, 重建 |
| ACTION | 10 秒 | 统计输出, 常规维护 |

---

## 12. 完整消息流示例

### 12.1 全量迁移

```
CLIENT                              SERVER
  │   connect ─────────────────────>│
  │                                  │
  │  [Frame: "abc" + Zstd(MsgPack)]─>│  解码 → 写入磁盘
  │<──────────── ACK(20B) ────────│
  │  [Frame: "abc" + Zstd(MsgPack)]─>│  解码 → 写入磁盘
  │<──────────── ACK(20B) ────────│
  │  ... (N 个块)                    │
  │                                  │
  │  close ────────────────────────>│
```

### 12.2 增量迁移

```
CLIENT                              SERVER
  │   connect ─────────────────────>│
  │                                  │
  │  [Frame: ctlIncremental] ──────>│  进入增量模式
  │                                  │
  │  [Frame: "abc" + Zstd(MsgPack)]─>│  解码 → 写入
  │<──────────── ACK(20B) ────────│
  │  (skip unchanged blocks...)     │
  │  [Frame: "abc" + Zstd(MsgPack)]─>│  解码 → 写入
  │<──────────── ACK(20B) ────────│
  │                                  │
  │  [Frame: ctlEndIncremental] ───>│  fsync 所有盘
  │<─────── SERVER_DONE(20B) ──────│
  │                                  │
  │  (下一轮增量...)                │
```

---

## 附录 A. 数据结构总览

```c
/* 块消息 (内存表示) */
typedef struct {
    int32_t  devno;        // 磁盘编号
    int64_t  offset;       // 字节偏移
    uint8_t *data;         // 数据指针
    uint32_t data_len;     // 数据长度 (≤ 1MB)
} block_message_t;

/* 服务端响应 */
typedef struct {
    int32_t  type;         // 响应类型
    int32_t  devno;        // 回显磁盘编号
    int32_t  size;         // 回显块大小
    int64_t  offset;       // 回显块偏移
} server_response_t;

/* MsgPack 类型标记 */
enum {
    MSGPACK_FIXMAP_3  = 0x93,
    MSGPACK_BIN8      = 0xc4,
    MSGPACK_BIN16     = 0xc5,
    MSGPACK_BIN32     = 0xc6,
    MSGPACK_INT8      = 0xd0,
    MSGPACK_INT16     = 0xd1,
    MSGPACK_INT32     = 0xd2,
    MSGPACK_INT64     = 0xd3,
};

/* 响应类型 */
enum {
    RESPONSE_ACK         = 0,
    RESPONSE_BINLOG      = 1,
    RESPONSE_SERVER_DONE = 2,
    RESPONSE_ACK_ALT     = 4,
};
```

## 附录 B. 编译选项

**服务端 (Linux)**:
```
gcc -O2 -Wall -o receiver server/*.c -Iinclude -lzstd -lpthread
```

**服务端 (MSVC)**:
```
cl /O2 /Fe:receiver.exe server\*.c /Iinclude /link libzstd.lib ws2_32.lib
```

**客户端 (MinGW)**:
```
gcc -O2 -Wall -o client.exe client/*.c -Iinclude \
    -lzstd -lsqlite3 -lole32 -lvssapi -lws2_32 -lpthread
```

**客户端 (MSVC)**:
```
cl /O2 /Fe:client.exe client\*.c /Iinclude \
    /link libzstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
```
