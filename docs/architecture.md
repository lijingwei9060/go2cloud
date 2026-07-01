# go2cloud 架构设计文档 v1.1

## 1. 概览

go2cloud 是一个**块级在线迁移工具**，将源端磁盘完整或增量地迁移到目标端。

### 1.1 核心特性

- **块级传输**: 数据以 1MB 固定块为单位，直接在磁盘扇区级别读取和写入
- **在线迁移**: 源端系统正常运行，通过 VSS 快照确保数据一致性
- **增量支持**: 基于块哈希的去重，后续轮次仅传输变化的块
- **并发传输**: 7 路 TCP 连接并发，充分利用网络带宽
- **压缩传输**: Zstd 压缩级别 1~7，减少网络传输量
- **断点续传**: SQLite 跟踪已确认块，支持中断后继续
- **崩溃一致性**: 最终副本等价于源端突然断电，NTFS 日志回放修复不一致

### 1.2 架构图

```
┌─────────────────────────────────┐    ┌─────────────────────────────────┐
│        SOURCE (客户端)           │    │       TARGET (服务端)            │
│                                  │    │                                  │
│  ┌──────────┐  ┌──────────────┐ │    │  ┌──────────────────────────────┐│
│  │  VSS     │  │  用户配置    │ │    │  │  服务端配置文件 (JSON)       ││
│  │ 快照管理 │  │  (user.json) │ │    │  └──────────────────────────────┘│
│  └──────────┘  └──────────────┘ │    │                                  │
│                                  │    │  ┌────────────────────────────┐  │
│  ┌──────────────────────────┐   │    │  │   Server Main (main.c)     │  │
│  │  Client Main (main.c)    │   │    │  │   - 配置解析               │  │
│  │  - 参数解析              │   │    │  │   - select 事件循环        │  │
│  │  - 迁移流程控制          │   │    │  │   - 信号处理               │  │
│  │  - 增量收敛判定          │   │    │  └────────────────────────────┘  │
│  └──────────────────────────┘   │    │                                  │
│                                  │    │  ┌────────────────────────────┐  │
│  ┌──────────┐ ┌──────────────┐  │    │  │  Session Manager           │  │
│  │ Volume   │ │ Block I/O    │  │    │  │  (session.c)               │  │
│  │ 卷枚举   │ │ 块读取       │  │    │  │  - 连接管理                │  │
│  └──────────┘ └──────────────┘  │    │  │  - 统计跟踪                │  │
│                                  │    │  └────────────────────────────┘  │
│  ┌──────────┐ ┌──────────────┐  │    │                                  │
│  │ Hash     │ │ MsgPack      │  │    │  ┌────────────────────────────┐  │
│  │ 块哈希   │ │ 编码器       │  │    │  │  Protocol Decoder          │  │
│  └──────────┘ └──────────────┘  │    │  │  (protocol_decoder.c)      │  │
│                                  │    │  │  - 4 层协议解码            │  │
│  ┌──────────┐ ┌──────────────┐  │    │  │  - MsgPack → Zstd → abc    │  │
│  │ Wire     │ │ Queue        │  │    │  └────────────────────────────┘  │
│  │ TCP 组帧 │ │ 发送队列     │  │    │                                  │
│  └──────────┘ └──────────────┘  │    │  ┌────────────────────────────┐  │
│                                  │    │  │  Block Writer              │  │
│  ┌──────────┐ ┌──────────────┐  │    │  │  (block_writer.c)          │  │
│  │ SQLite   │ │ Pool         │  │    │  │  - 磁盘写入 + fsync        │  │
│  │ 跟踪 DB  │ │ 连接池       │  │    │  │  - binlog 大小监控         │  │
│  └──────────┘ └──────────────┘  │    │  └────────────────────────────┘  │
│                                  │    │                                  │
│  ┌──────────┐ ┌──────────────┐  │    │  ┌────────────────────────────┐  │
│  │ Timer    │ │ Log          │  │    │  │  ACK Generator             │  │
│  │ 定时器   │ │ 日志         │  │    │  │  (ack.c)                   │  │
│  └──────────┘ └──────────────┘  │    │  │  - 20B 响应                │  │
│                                  │    │  │  - BINLOG 告警             │  │
│           │ 7 TCP Connections    │    │  └────────────────────────────┘  │
│           └──────────────────────┼────┼───────────┘                      │
│                                  │    │                                  │
└─────────────────────────────────┘    └─────────────────────────────────┘
```

---

## 2. 客户端模块

### 2.1 模块清单

| 模块 | 文件 | 职责 |
|------|------|------|
| main | main.c | 入口，流程控制，子命令，增量收敛判定 |
| log | log.c / log.h | 线程安全日志 |
| hash | hash.c / hash.h | 自定义 64-bit 哈希 |
| msgpack | msgpack.c / msgpack.h | MsgPack 编码器 |
| wire | wire.c / wire.h | TCP 有线协议 (4 层封装) |
| queue | queue.c / queue.h | 线程安全环形发送队列 |
| pool | pool.c / pool.h | Socket 连接池 (7 并发) |
| timer | timer.c / timer.h | 定时器管理 (重传/增量/重连/动作) |
| sqlite | sqlite.c / sqlite.h | 块去重跟踪数据库 (T_BLOCK) |
| volume | volume.c / volume.h | 磁盘卷枚举 |
| block_io | block_io.c / block_io.h | 块级磁盘读取 (VSS / PhysicalDrive) |
| vss | vss.c / vss.h | VSS 快照管理 (创建/查询/删除) |

### 2.2 数据流

```
  Volume Enum → Block Read → Hash Calc → SQLite Lookup
       │             │            │             │
       │             │            │        [skip if unchanged]
       │             │            │             │
       │             └────────────┴─────────────┘
       │                        │
       │                  MsgPack Encode
       │                        │
       │                  Queue Push
       │                        │
       │                  Queue Pop (network thread)
       │                        │
       │                  Wire Send (Zstd + abc + TCP frame)
       │                        │
       │                  Pool (7 TCP connections)  ────> SERVER
       │                                                   │
       ├──── ACK receive <─────────────────────────────────┘
       │         │
       │    Queue Ack + SQLite mark (TailSend: skip mark)
       │
       └──── 18s Incremental Timer:
                SQLite get_unacked → shuffle → live disk read
                → hash compare → [changed? resend : mark ack=1]
```

### 2.3 迁移流程

```
 1. 解析命令行参数
 2. 读取 user.json 配置
 3. 初始化日志
 4. 枚举磁盘卷 (Windows: PhysicalDriveN)
 5. 创建 VSS 快照 → 获取快照设备路径
 6. 打开读取器 (VSS 优先 + TailSend 时额外打开 live reader)
 7. 初始化连接池 → 连接服务端 (最多 6 次重试)
 8. 初始化 SQLite → 打开 tracker.db
 9. [TailSend] 发送 ctlIncremental 控制帧
10. 初始化定时器 (重传/增量/重连/动作)
11. 全量传输循环:
    a. 遍历每个分区
    b. 遍历每个块 (offset = 0, 1MB, 2MB, ...)
    c. 读块 → 计算哈希 → SQLite 去重
    d. [TailSend] 跳过已确认块
    e. MsgPack 编码 → 入队 → 发送
    f. 处理 ACK ([TailSend] 不标记 ack=1)
    g. 定时器检查:
       - TIMER_INCREMENTAL: 增量轮次 (从 live disk 重读)
       - TIMER_RETRANSMIT: 重传检查
       - TIMER_RECONNECT: 连接管理
       - TIMER_ACTION: 进度日志
12. 排空队列
13. [TailSend] 增量收敛循环:
    a. 每轮 18s: sqlite_reset_acked → 查询未确认 → 洗牌 → 读 live disk → 哈希对比
    b. hash 相同 → mark_acked (稳定)
    c. hash 不同 → 重发
    d. should_finish 判定 (完美/稳定/超时)
14. 发送 ctlEndIncremental
15. 等待 SERVER_DONE (服务端 fsync 完成)
16. sqlite_clear_all_blocks → vss_cleanup → 退出
```

### 2.4 增量同步模型

go2cloud 采用**双层增量**设计（比原始 go2tencentcloud 更彻底）：

```
          全量同步 (VSS)               增量收敛循环 (live disk)
  ────────────────────────────    ─────────────────────────────────
  │                          │    │                                │
  │  18s 并发增量            │    │  18s 独立增量轮次              │
  │  (仅处理 ack=0 的块)     │    │  (reset ack → 重读所有块)     │
  │                          │    │                                │
  ────────────────────────────    ─────────────────────────────────
  time ─────────────────────────────────────────────────────────────→
```

- **全量期间**: 18s 定时器处理飞行中的块 (已发送未确认)，从 live disk 重读并对比哈希
- **全量后**: 进入独立收敛循环，每轮重置所有 ack=1 → ack=0，全部块重新评估

原始 go2tencentcloud 只有第一层（全量期间并发），全量完成后立即结束。

---

## 3. 服务端模块

### 3.1 模块清单

| 模块 | 文件 | 职责 |
|------|------|------|
| main | main.c | TCP 服务器，事件循环，配置 |
| log | log.c / log.h | 线程安全日志 |
| session | session.c / session.h | 客户端连接管理 |
| protocol_decoder | protocol_decoder.c / protocol_decoder.h | 4 层协议解码器 |
| block_writer | block_writer.c / block_writer.h | 目标磁盘块写入 + binlog |
| ack | ack.c / ack.h | 20 字节 ACK 响应 |

### 3.2 请求处理流程

```
  recv() → 粘包缓冲 (recv_buf)
     │
     ├─ 拆包: 累积直到收到完整帧
     │
     ├─ 控制帧检测: 帧数据不以 "abc" 开头
     │    ├─ "ctlIncremental"  → 标记增量模式
     │    └─ "ctlEndIncremental" → fsync_all + ACK SERVER_DONE
     │
     └─ 数据帧: 帧数据以 "abc" 开头
          ├─ Zstd 解压
          ├─ MsgPack 解码
          │    ├─ fixmap(3) 验证
          │    ├─ key0 → devno
          │    ├─ key1 → offset
          │    └─ key2 → bin data
          ├─ Block Writer 写入磁盘
          ├─ BINLOG 大小检查 (≥ 10GB → 告警)
          └─ ACK 响应
```

### 3.3 BINLOG 监控

服务端在 `handle_session` 中检查 binlog 大小：
- `block_writer_pending_bytes()` 原子读取并重置计数器
- 超过 `BINLOG_SIZE_LIMIT` (10GB) → 发送 `RESPONSE_BINLOG` 告警
- 客户端收到告警后退出 (`exit(0)`)

---

## 4. 目录结构

```
go2cloud/
├── include/
│   └── protocol.h           # 共享协议定义 (4 层协议栈常量)
├── client/
│   ├── main.c               # 客户端入口 + 流程控制
│   ├── log.h / log.c        # 日志模块
│   ├── hash.h / hash.c      # 哈希模块
│   ├── msgpack.h / msgpack.c # MsgPack 编码
│   ├── wire.h / wire.c      # TCP 有线协议
│   ├── queue.h / queue.c    # 发送队列
│   ├── pool.h / pool.c      # 连接池
│   ├── timer.h / timer.c    # 定时器
│   ├── sqlite.h / sqlite.c  # SQLite 块跟踪
│   ├── volume.h / volume.c  # 卷枚举
│   ├── block_io.h / block_io.c # 块读取
│   └── vss.h / vss.c        # VSS 快照
├── server/
│   ├── main.c               # 服务端入口
│   ├── log.h / log.c        # 日志模块
│   ├── session.h / session.c # 会话管理
│   ├── protocol_decoder.h / protocol_decoder.c # 协议解码
│   ├── block_writer.h / block_writer.c # 块写入 + binlog
│   └── ack.h / ack.c        # ACK 响应
├── test/
│   └── test_protocol.c      # 协议测试
├── docs/
│   ├── client.md             # 客户端设计文档
│   ├── architecture.md       # 本文档
│   ├── protocol.md           # 协议规范
│   ├── modules.md            # 模块接口文档
│   └── cutover.md            # 割接流程
└── Makefile                  # 构建系统
```

---

## 5. 配置格式

### 5.1 客户端 (user.json)

```json
{
  "Log": {
    "Level": 2,
    "Path": "client.log"
  },
  "TailSend": 0,
  "HasDump": 1,
  "DbPath": "tracker.db",
  "SkipDisks": [],
  "Disks": {
    "0": "\\\\.\\PhysicalDrive0",
    "1": "\\\\.\\PhysicalDrive1"
  }
}
```

### 5.2 服务端 (receiver.json)

```json
{
  "Listen": {
    "Address": "0.0.0.0",
    "Port": 3389,
    "MaxConnections": 7
  },
  "Target": {
    "Disks": {
      "0": "/dev/sdb",
      "1": "/dev/sdc"
    }
  },
  "Log": {
    "Level": 2,
    "Path": "/var/log/receiver.log"
  }
}
```

---

## 6. 错误处理与可靠性

### 6.1 重传机制

- 每 18 秒扫描 SQLite 中 `ack=0` 的块
- 对超过 121 秒仍未确认的块进行重传
- 使用 Fisher-Yates 随机打乱重传顺序避免拥塞

### 6.2 连接恢复

- 每 20 秒检测过期连接 (> 12 分钟无活动)
- 自动关闭过期连接并重建
- 新连接自动附加到连接池
- 首次连接最多重试 6 次

### 6.3 背压控制

- 全量发送: 队列深度 > 40 块 → 暂停读取新块，Sleep(50ms)
- 增量发送: 队列深度 > 9 块 → 暂停本轮，剩余留待下轮
- 内存使用 > 45MB → Sleep(50ms)
- VSS 快照失败 → 回退到直接读取 PhysicalDrive
- 磁盘写入失败 → 记录日志，不发送 ACK，触发客户端重传

### 6.4 断点续传

- SQLite 持久化所有已发送块 (devno + offset + hash)
- 程序重启后，跳过 `ack=1` 的块（已确认）
- 仅重传 `ack=0` 的块（未确认）

### 6.5 崩溃一致性保证

- VSS 快照提供全量同步的静态时间点基准
- 18s 并发增量捕获全量期间的写入变化
- 全量后独立增量收敛循环进一步缩小数据差异窗口
- 最终目标端数据 = 源端"最后增量轮 + 未确认块"之前的状态
- NTFS 日志回放（目标启动时）修复文件系统级不一致
- 等价于源端突然断电的场景，保证数据可恢复性
