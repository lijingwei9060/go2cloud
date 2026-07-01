# go2cloud 服务端 (receiver) 使用手册

## 1. 概述

`receiver` 是 go2cloud 块级迁移工具的目标端程序，负责：

- 监听 TCP 端口，接受客户端连接
- 解码 4 层有线协议（TCP 分帧 → "abc" 同步魔数 → Zstd 解压 → MsgPack 解码）
- 将块数据写入目标磁盘（支持多盘）
- 返回 20 字节 ACK 响应
- 处理增量同步协议（`ctlIncremental` / `ctlEndIncremental`）
- Binlog 大小监控，溢出告警
- 支持 Linux 和 Windows 双平台

receiver 是**无状态**的 — 不存储元数据，不维护块跟踪数据库。所有状态由客户端 SQLite 管理。

---

## 2. 命令行

```
./receiver --config receiver.json
./receiver --help
```

| 参数 | 说明 |
|------|------|
| `--config <path>` | 配置文件路径，默认 `receiver.json` |
| `--help`, `-h` | 显示帮助 |

如果配置文件不存在，receiver 使用默认值启动：监听 `0.0.0.0:8889`，写入 `/tmp/receiver_disk0.img`。

---

## 3. 配置文件 (receiver.json)

```json
{
    "Listen": {
        "Address": "0.0.0.0",
        "Port": 8889,
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

### 3.1 配置字段

#### Listen — 监听配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `Address` | string | `"0.0.0.0"` | 监听地址，`0.0.0.0` 表示所有网卡 |
| `Port` | int | `8889` | 监听端口 |
| `MaxConnections` | int | `7` | 最大并发客户端连接数 |

> 客户端使用 7 路 TCP 连接池并发传输，`MaxConnections` 应 ≥ `SOCKET_POOL_TARGET` (7)。

#### Target.Disks — 目标磁盘映射

| 字段 | 类型 | 说明 |
|------|------|------|
| `"0"`, `"1"`, ... `"15"` | string | 磁盘编号到设备路径的映射 |

- `devno` 与客户端 `client.exe info` 输出中的 `Disk` 列对应
- 路径必须是预先存在且大小正确的块设备或文件
- receiver **不会创建磁盘** — 磁盘由 Go 控制端或运维人员预先准备
- 最大支持 16 个磁盘（`MAX_TARGET_DISKS`）

#### Log — 日志配置

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `Level` | int | `2` (INFO) | 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR |
| `Path` | string | `""` (stderr) | 日志文件路径 |

### 3.2 配置示例

**Linux 迁移到物理磁盘：**
```json
{
    "Listen": {"Address": "0.0.0.0", "Port": 8889, "MaxConnections": 7},
    "Target": {"Disks": {"0": "/temp/i-11111111/sdb", "1": "/temp/i-11111111/sdc"}},
    "Log": {"Level": 2, "Path": "/var/log/receiver.log"}
}
```

**测试环境（写入稀疏文件）：**
```json
{
    "Listen": {"Address": "127.0.0.1", "Port": 8889, "MaxConnections": 7},
    "Target": {"Disks": {"0": "/tmp/target_disk0.img"}},
    "Log": {"Level": 1, "Path": "receiver.log"}
}
```

---

## 4. 编译

### 4.1 Linux

**安装依赖**：

```sh
# Ubuntu / Debian
sudo apt install build-essential libzstd-dev

# RHEL / CentOS / Fedora
sudo yum install gcc make zstd-devel
```

| 包 | 用途 |
|---|------|
| `build-essential` | gcc + libc + POSIX 头文件（`sys/socket.h`、`pthread.h` 等） |
| `libzstd-dev` | Zstd 解压（`protocol_decoder.c` 引用 `<zstd.h>`） |

> receiver 不需要 sqlite3 — 块跟踪和去重完全由客户端管理，服务端无状态。

**编译**：

```sh
gcc -O2 -Wall -o receiver server/*.c -Iinclude -lzstd -lpthread
```

### 4.2 Windows (MSVC)

```bat
cl /O2 /utf-8 /Fe:receiver.exe server\*.c /Iinclude ^
   /I<vcpkg>\installed\x64-windows\include ^
   /link libzstd.lib ws2_32.lib
```

---

## 5. 目标磁盘准备

receiver **不创建磁盘**，目标设备必须预先存在。

### 5.1 使用真实块设备

```sh
# 确认目标磁盘存在
lsblk

# 确保大小 ≥ 源端磁盘
fdisk -l /dev/sdb

# receiver 直接写入 (需要 root 权限)
sudo ./receiver --config receiver.json
```

### 5.2 使用稀疏文件（测试）

```sh
# 创建与源端相同大小的稀疏文件
truncate -s 50G /tmp/target_disk0.img

# receiver.json
# {"Target": {"Disks": {"0": "/tmp/target_disk0.img"}}}

./receiver --config receiver.json
```

### 5.3 磁盘编号对应关系

客户端 `client.exe info` 输出的第一列：

```
Disk   Size(GB) TotalBlocks      Name             DiskPath
------ -------- ---------------- ---------------- ----
0      50.00    51200            C:               \\.\PhysicalDrive0
1      100.00   102400           D:               \\.\PhysicalDrive1
```

receiver 配置中 `"0"` → 目标端对应源端 Disk 0 的设备，`"1"` → 对应源端 Disk 1 的设备。

---

## 6. 运行时

### 6.1 启动流程

```
1. 读取 receiver.json
2. 初始化日志
3. 解析 Listen 配置
4. 初始化 session 管理器
5. 初始化 block_writer
6. 解析 Target.Disks → register
7. 打开所有目标磁盘 (open)
8. 创建监听 socket → bind → listen
9. 注册 SIGINT/SIGTERM 信号处理
10. 进入 select 事件循环
```

### 6.2 启动输出示例

```
go2cloud receiver starting...
Target disks opened successfully
Listening on 0.0.0.0:8889 (max 7 connections)
```

### 6.3 连接处理

对每个客户端连接：

1. `accept()` 新连接 → `session_create(fd, remote_addr)`
2. 设 socket 为非阻塞模式
3. 最多 `MaxConnections` 个并发连接，超出拒绝

### 6.4 请求处理流程

```
recv() → session.recv_buf (4MB 粘包缓冲区)
    │
    ├─ 拆帧: 读 4B 大端长度 → 累计数据直到完整帧
    │
    ├─ 控制帧: payload 不以 "abc" 开头
    │    ├─ "ctlIncremental"  → session.incremental = 1
    │    └─ "ctlEndIncremental" → fsync_all() + ack_send_done(type=2)
    │
    └─ 数据帧: payload 以 "abc" 开头
         ├─ 跳过 "abc" 魔数 (3B)
         ├─ Zstd 解压
         ├─ MsgPack 解码
         │    ├─ fixmap(3) 验证
         │    ├─ key0 → devno  (int)
         │    ├─ key1 → offset (int64)
         │    └─ key2 → data   (bin)
         ├─ block_writer_write(devno, offset, data)
         │    ├─ lseek(fd, offset)
         │    └─ write(fd, data, len)  (循环处理短写)
         ├─ 每 2000 块周期性 fsync
         ├─ BINLOG 检查: pending_bytes ≥ 10GB → ack_send_binlog(type=1)
         └─ ack_send_block(type=0) — 20B 响应
```

### 6.5 停止

```sh
# 优雅停止
kill -TERM <pid>
# 或 Ctrl+C
```

停止流程：
1. 接收信号 → `g_running = 0`
2. `select()` 循环退出
3. 关闭监听 socket
4. `block_writer_fsync_all()` — 刷新所有目标磁盘
5. `block_writer_close_all()` — 关闭所有文件描述符
6. `log_close()` — 关闭日志

---

## 7. ACK 响应格式

所有响应是 20 字节原始结构体，不经编码直接 TCP 发送：

```
Byte  0-3:  type   (int32)   — 响应类型
Byte  4-7:  devno  (int32)   — 磁盘编号 (回显)
Byte  8-11: size   (int32)   — 块大小 (回显)
Byte 12-19: offset (int64)   — 块偏移 (回显)
```

| type | 宏 | 含义 |
|------|-----|------|
| 0 | `RESPONSE_ACK` | 块已持久化写入 |
| 1 | `RESPONSE_BINLOG` | Binlog 溢出告警 (≥ 10GB) |
| 2 | `RESPONSE_SERVER_DONE` | `ctlEndIncremental` 后 fsync 完成 |
| 4 | `RESPONSE_ACK_ALT` | 块确认变体 (同 type=0) |

---

## 8. Binlog 监控

receiver 跟踪自上次 BINLOG 通知以来的待处理字节数：

- `g_pending_bytes` 累积每次 `block_writer_write()` 的写入量
- 每次 BINLOG 检查时通过 `block_writer_pending_bytes()` **原子读取并清零**
- 达到 `BINLOG_SIZE_LIMIT` (10GB) → 发送 `RESPONSE_BINLOG` 告警
- 客户端收到告警后 `exit(0)` 退出迁移

**设计意图**：防止目标端积压过多未 fsync 的数据。10GB 阈值对应原始 go2tencentcloud 的 `0x280000000ULL`。

---

## 9. 文件布局

```
server/
├── main.c               # 入口: 配置解析, TCP 服务器, select 事件循环
├── log.h / log.c        # 线程安全日志 (跨平台)
├── session.h / session.c # 客户端会话管理 (最多 16 个, 4MB recv_buf)
├── protocol_decoder.h / protocol_decoder.c  # 4 层协议解码器
├── block_writer.h / block_writer.c          # 目标磁盘写入 + binlog
└── ack.h / ack.c        # 20 字节 ACK 响应生成
```

### 9.1 模块接口

| 模块 | 关键函数 | 说明 |
|------|---------|------|
| `session` | `session_create`, `session_remove`, `session_foreach` | 连接生命周期管理 |
| `protocol_decoder` | `protocol_decode` | 累积式解码，支持粘包/半包 |
| `block_writer` | `block_writer_register`, `block_writer_write`, `block_writer_fsync_all` | 目标盘 I/O |
| `ack` | `ack_send_block`, `ack_send_done`, `ack_send_binlog` | 20B 响应 |

---

## 10. 完整迁移时序

```
receiver (目标端)                          client (源端)
─────────────────                          ──────
1. 启动, 打开目标盘, 监听 8889
                                          2. 启动, 创建 VSS 快照
3. accept() ←──────────────────────────── 4. 连接 (7 TCP connections)
                                          5. [TailSend] ctlIncremental →
6. session.incremental = 1
                                          7. 全量同步: 逐块发送
8. 每块: decode → write → fsync → ACK →
                                          9. 处理 ACK, 更新 SQLite
                                          10. 18s 增量定时器: 并发重读 live disk
                                          11. 全量完成 → 增量收敛循环
                                          12. ctlEndIncremental →
13. fsync_all() → SERVER_DONE →
                                          14. allDone → DELETE T_BLOCK → exit(0)
15. 连接关闭, 等待新连接或 SIGTERM
```

---

## 11. 故障处理

| 场景 | receiver 行为 |
|------|-------------|
| 目标盘不存在 | `open()` 失败，启动报错退出 |
| 目标盘空间不足 | `write()` 失败，记录日志，客户端重传 |
| 客户端断连 | `session_remove()` 清理，不影响其他连接 |
| 超出最大连接数 | 拒绝新连接，记录 WARN 日志 |
| `select()` 被信号中断 | `EINTR` → 重新进入循环 |
| BINLOG ≥ 10GB | 发送告警，客户端退出，等待 Go 端重试 |

---

## 12. 与客户端协议常量对照

| 常量 | 值 | 说明 |
|------|-----|------|
| `BLOCK_SIZE` | 1MB (`0x100000`) | 标准块大小 |
| `TCP_FRAME_HEADER_SIZE` | 4 | 帧长度前缀 |
| `FRAME_MAGIC` | `"abc"` | 帧同步魔数 |
| `SERVER_RESPONSE_SIZE` | 20 | ACK 响应长度 |
| `BINLOG_SIZE_LIMIT` | 10GB (`0x280000000`) | Binlog 告警阈值 |
| `MAX_TARGET_DISKS` | 16 | 最大目标磁盘数 |
| `MAX_SESSIONS` | 16 | 最大并发会话数 |
| `FSYNC_BLOCK_INTERVAL` | 2000 | 每 2000 块 (~2GB) 执行 fsync |
