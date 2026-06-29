# go2cloud 架构设计文档 v1.0

## 1. 概览

go2cloud 是一个**块级在线迁移工具**, 将源端磁盘完整或增量地迁移到目标端。

### 1.1 核心特性

- **块级传输**: 数据以 1MB 固定块为单位, 直接在磁盘扇区级别读取和写入
- **在线迁移**: 源端系统正常运行, 通过 VSS 快照确保数据一致性
- **增量支持**: 基于块哈希的去重, 后续轮次仅传输变化的块
- **并发传输**: 7 路 TCP 连接并发, 充分利用网络带宽
- **压缩传输**: Zstd 压缩级别 1~7, 减少网络传输量
- **断点续传**: SQLite 跟踪已确认块, 支持中断后继续

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
│  └──────────────────────────┘   │    │  └────────────────────────────┘  │
│                                  │    │                                  │
│  ┌──────────┐ ┌──────────────┐  │    │  ┌────────────────────────────┐  │
│  │ Volume   │ │ Block I/O    │  │    │  │  Session Manager           │  │
│  │ 卷枚举   │ │ 块读取       │  │    │  │  (session.c)               │  │
│  └──────────┘ └──────────────┘  │    │  │  - 连接管理                │  │
│                                  │    │  │  - 统计跟踪                │  │
│  ┌──────────┐ ┌──────────────┐  │    │  └────────────────────────────┘  │
│  │ Hash     │ │ MsgPack      │  │    │                                  │
│  │ 块哈希   │ │ 编码器       │  │    │  ┌────────────────────────────┐  │
│  └──────────┘ └──────────────┘  │    │  │  Protocol Decoder          │  │
│                                  │    │  │  (protocol_decoder.c)      │  │
│  ┌──────────┐ ┌──────────────┐  │    │  │  - 4 层协议解码            │  │
│  │ Wire     │ │ Queue        │  │    │  │  - MsgPack → Zstd → abc    │  │
│  │ TCP 组帧 │ │ 发送队列     │  │    │  └────────────────────────────┘  │
│  └──────────┘ └──────────────┘  │    │                                  │
│                                  │    │  ┌────────────────────────────┐  │
│  ┌──────────┐ ┌──────────────┐  │    │  │  Block Writer              │  │
│  │ SQLite   │ │ Pool         │  │    │  │  (block_writer.c)          │  │
│  │ 跟踪 DB  │ │ 连接池       │  │    │  │  - 磁盘写入 + fsync        │  │
│  └──────────┘ └──────────────┘  │    │  └────────────────────────────┘  │
│                                  │    │                                  │
│  ┌──────────┐ ┌──────────────┐  │    │  ┌────────────────────────────┐  │
│  │ Timer    │ │ Log          │  │    │  │  ACK Generator             │  │
│  │ 定时器   │ │ 日志         │  │    │  │  (ack.c)                   │  │
│  └──────────┘ └──────────────┘  │    │  │  - 20B 响应                │  │
│                                  │    │  └────────────────────────────┘  │
│           │ 7 TCP Connections    │    │           ▲                      │
│           └──────────────────────┼────┼───────────┘                      │
│                                  │    │                                  │
└─────────────────────────────────┘    └─────────────────────────────────┘
```

---

## 2. 客户端模块

### 2.1 模块清单

| 模块 | 文件 | 职责 |
|------|------|------|
| main | main.c | 入口, 流程控制, 子命令 |
| log | log.c / log.h | 线程安全日志 |
| hash | hash.c / hash.h | 自定义 64-bit 哈希 |
| msgpack | msgpack.c / msgpack.h | MsgPack 编码器 |
| wire | wire.c / wire.h | TCP 有线协议 (4 层封装) |
| queue | queue.c / queue.h | 线程安全环形发送队列 |
| pool | pool.c / pool.h | Socket 连接池 (7 并发) |
| timer | timer.c / timer.h | 定时器管理 |
| sqlite | sqlite.c / sqlite.h | 块去重跟踪数据库 |
| volume | volume.c / volume.h | 磁盘卷枚举 |
| block_io | block_io.c / block_io.h | 块级磁盘读取 |
| vss | vss.c / vss.h | VSS 快照管理 |

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
       └─── ACK receive <──────────────────────────────────┘
                 │
            Queue Ack + SQLite mark
```

### 2.3 迁移流程

```
1. 解析命令行参数
2. 读取 user.json 配置
3. 初始化日志
4. 枚举磁盘卷 (Windows: PhysicalDriveN)
5. [可选] 创建 VSS 快照
6. 初始化连接池 → 连接服务端
7. 初始化 SQLite → 打开 tracker.db
8. [增量] 发送 ctlIncremental
9. 主传输循环:
   a. 遍历每个磁盘
   b. 遍历每个块 (offset = 0, 1MB, 2MB, ...)
   c. 读取块数据
   d. 计算哈希
   e. SQLite 查询去重 (增量模式)
   f. MsgPack 编码
   g. 入队列
   h. 出队列 → Wire 发送
   i. 接收 ACK → 标记完成
   j. 定时器: 重传/重连/统计
10. [增量] 发送 ctlEndIncremental
11. 等待 SERVER_DONE
12. 清理退出
```

---

## 3. 服务端模块

### 3.1 模块清单

| 模块 | 文件 | 职责 |
|------|------|------|
| main | main.c | TCP 服务器, 事件循环, 配置 |
| log | log.c / log.h | 线程安全日志 |
| session | session.c / session.h | 客户端连接管理 |
| protocol_decoder | protocol_decoder.c / protocol_decoder.h | 4 层协议解码器 |
| block_writer | block_writer.c / block_writer.h | 目标磁盘块写入 |
| ack | ack.c / ack.h | 20 字节 ACK 响应 |

### 3.2 请求处理流程

```
  recv() → 粘包缓冲 (recv_buf)
     │
     ├─ 拆包: 累积直到收到完整帧
     │
     ├─ 控制帧检测: 帧数据不以 "abc" 开头
     │    ├─ "ctlIncremental"  → session.incremental = 1
     │    └─ "ctlEndIncremental" → fsync_all + ACK done
     │
     └─ 数据帧: 帧数据以 "abc" 开头
          ├─ Zstd 解压
          ├─ MsgPack 解码
          │    ├─ fixmap(3) 验证
          │    ├─ key0 → devno
          │    ├─ key1 → offset
          │    └─ key2 → bin data
          ├─ Block Writer 写入磁盘
          └─ ACK 响应
```

---

## 4. 目录结构

```
go2cloud/
├── include/
│   └── protocol.h           # 共享协议定义
├── client/
│   ├── main.c               # 客户端入口
│   ├── log.h / log.c        # 日志模块
│   ├── hash.h / hash.c      # 哈希模块
│   ├── msgpack.h / msgpack.c # MsgPack 编码
│   ├── wire.h / wire.c      # TCP 有线协议
│   ├── queue.h / queue.c    # 发送队列
│   ├── pool.h / pool.c      # 连接池
│   ├── timer.h / timer.c    # 定时器
│   ├── sqlite.h / sqlite.c  # SQLite 跟踪
│   ├── volume.h / volume.c  # 卷枚举
│   ├── block_io.h / block_io.c # 块读取
│   └── vss.h / vss.c        # VSS 快照
├── server/
│   ├── main.c               # 服务端入口
│   ├── log.h / log.c        # 日志模块
│   ├── session.h / session.c # 会话管理
│   ├── protocol_decoder.h / protocol_decoder.c # 协议解码
│   ├── block_writer.h / block_writer.c # 块写入
│   └── ack.h / ack.c        # ACK 响应
├── test/
│   └── test_protocol.c      # 协议测试
├── docs/
│   ├── protocol.md           # 协议规范
│   ├── architecture.md       # 本文档
│   └── modules.md            # 模块接口文档
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

### 6.3 背压控制

- 发送队列深度 > 40 块 → 暂停读取新块, Sleep(50ms)
- VSS 快照失败 → 回退到直接读取源盘
- 磁盘写入失败 → 记录日志, 不发送 ACK, 触发客户端重传

### 6.4 断点续传

- SQLite 持久化已确认的块 (devno + offset)
- 程序重启后, 跳过 `ack=1` 的块
- 仅重传 `ack=0` 的块
