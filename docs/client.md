# go2cloud 客户端 (client.exe) 设计文档

## 1. 概述

`client.exe` 是 go2cloud 块级迁移工具的数据面程序，负责：

- 枚举源端物理磁盘分区
- 通过 VSS 创建一致性快照
- 逐块读取、哈希、压缩、发送磁盘数据
- 接收 ACK 确认，支持崩溃恢复
- 增量同步: 从 live disk 重读块、对比哈希、重发变化的块
- 通过子命令向 Go 控制端提供磁盘/固件/进度信息

`client.exe` 可被 **Go 控制端**（通过 `CreateProcess`）或 **用户直接**调用。

### 1.1 原始参考 (go2tencentcloud)

本实现基于对腾讯云 go2tencentcloud `client.exe` 的逆向分析（Ghidra 反编译）重写。关键差异见第 8 节。

---

## 2. 命令行模式

### 2.1 子命令模式（信息采集 / 会话控制）

```
client.exe info                   # 显示磁盘分区信息
client.exe hash <file>            # 计算文件哈希
client.exe check <disk>           # 检查磁盘可访问性
client.exe isbios                 # 检测固件类型
client.exe begin_session          # 标记迁移会话开始
client.exe end_session            # 清理块跟踪数据库
client.exe sentbytes              # 查询已确认总字节数
client.exe test_vss               # 测试 VSS 快照功能（创建持久快照）
client.exe vss_query              # 查询所有现有快照
client.exe vss_delete <guid>      # 删除指定快照
client.exe vss_delete --all       # 删除所有快照
client.exe dryrun [config.json]   # 本地模拟全量+增量迁移，无网络 I/O
client.exe --help                 # 显示帮助
```

### 2.2 迁移模式（数据面传输）

```
client.exe <server_ip:port> [config.json]
```

---

## 3. 子命令详解

### 3.1 info — 分区信息

```
client.exe info
```

枚举所有固定磁盘上的分区，按盘符（如果有）和组织名称显示。每个条目是一个卷/分区（非物理磁盘），`Disk` 列表示所在物理磁盘编号。输出格式：

```
Volume Information:
Disk   Size(GB) TotalBlocks      Name             DiskPath
------ -------- ---------------- ---------------- ----
0      50.00    51200            C:               \\.\PhysicalDrive0
0      0.53     544              Disk0Part3       \\.\PhysicalDrive0
1      100.00   102400           D:               \\.\PhysicalDrive1
```

**权限**：需要管理员权限（`CreateFile(\\.\PhysicalDriveN)` 要求）。

**内部步骤**：
1. 枚举 `\\.\PhysicalDriveN`（N=0..15），过滤可移动介质和 CD-ROM
2. 通过 `IOCTL_DISK_GET_DRIVE_LAYOUT_EX` 获取 GPT/MBR 分区布局
3. 通过 `FindFirstVolumeW` + `GetVolumePathNamesForVolumeNameW` + `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` 匹配盘符
4. 跳过 MSR 分区（GPT 保留分区）

### 3.2 isbios — 固件类型检测

```
client.exe isbios
```

输出一行：`UEFI` 或 `Legacy`。Go 端用于确定目标实例配置（UEFI → 需要 EFI 分区，Legacy → MBR 引导）。

**内部步骤**：
1. 启用 `SeSystemEnvironmentPrivilege`（`OpenProcessToken` + `AdjustTokenPrivileges`）
2. 调用 `GetFirmwareType()`（Windows 8+）
3. 回退：`GetFirmwareEnvironmentVariableA` 探测 UEFI 命名空间

**退出码**：始终 0。

### 3.3 test_vss — VSS 功能验证 (持久快照)

```
client.exe test_vss
```

创建 `C:` 卷的**持久快照**（`VSS_CTX_CLIENT_ACCESSIBLE`，与原始 go2tencentcloud 行为一致）。

**成功输出**：
```
Using VSS volume: \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy5
```
退出码 0。快照在进程退出后继续存在，需要用 `vss_delete` 清理。

**失败输出**：
```
VSS init failed
```
退出码 1。

常见失败原因：
- 非管理员权限（`E_ACCESSDENIED`）
- 系统不支持 VSS（非 Windows Server/Pro）
- 磁盘空间不足（持久快照需要 diff area）
- `VSS_E_UNSUPPORTED_CONTEXT (0x80042316)` — 系统不支持 `VSS_CTX_CLIENT_ACCESSIBLE`

**内部步骤**：
1. `vss_init_ex(VSS_CTX_CLIENT_ACCESSIBLE)` — 加载 vssapi.dll，初始化 COM，`InitializeForBackup` 后调用 `SetContext(0x1d)`
2. `vss_create_snapshots(ctx, {"C:\\", NULL})` — 创建持久快照（跳过 `GatherWriterMetadata` 和 `PrepareForBackup`）
3. 打印快照设备路径
4. `vss_backup_complete()` + `vss_cleanup()` — 进程退出，快照保留

### 3.4 vss_query — 查询快照

```
client.exe vss_query
```

列出系统上所有 `ClientAccessible` 类型的持久快照。输出格式：

```
Shadow Copy ID: {a8eda6c5-8d6d-43ff-a022-25010767481a}
  Original Volume: \\?\Volume{7f6da094-ef53-45fb-a850-7380ba52e6d4}\
  Device Object:   \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy5
  Creation Time:   2026-07-01 13:56:21
```

退出码 0（即使无快照）。

### 3.5 vss_delete — 删除快照

```
client.exe vss_delete {guid}      # 删除指定 GUID 对应的快照
client.exe vss_delete --all       # 删除所有快照
```

删除后输出已删除数量：
```
Deleted 1 snapshot(s).
```

退出码 0（成功）或 1（失败）。

**注意**：Go 控制端在迁移完成后通过此命令清理 `test_vss` 创建的持久快照。

### 3.6 sentbytes — 跨进程进度查询

```
client.exe sentbytes
```

输出一行整数：已确认块的总字节数。

```
2097152
```

**跨进程原理**：迁移进程通过 SQLite WAL 模式持续更新 `T_BLOCK` 表的 `ack=1` 标记。Go 端轮询 `client.exe sentbytes`（作为独立进程），该命令打开同一 `tracker.db`，执行 `SELECT COALESCE(SUM(size), 0) FROM T_BLOCK WHERE ack=1`。

**退出码**：0（即使数据库不存在，输出 0 后返回 1）。

### 3.7 begin_session / end_session — 会话控制

```
client.exe begin_session        # 退出码 0（无操作标记）
client.exe end_session          # 清理 tracker.db → DELETE FROM T_BLOCK
```

**`begin_session`**：Go 端在启动数据面进程前调用。确认 C 端可执行文件存在且可运行。

**`end_session`**：迁移完成或取消后调用。执行 `DELETE FROM T_BLOCK` 清空块跟踪表。**注意**：不释放 VSS 快照 — 快照由数据面进程退出时随进程销毁。

### 3.8 check / hash — 辅助诊断

```
client.exe check \\.\PhysicalDrive0          # 检查磁盘可读性
client.exe hash C:\data\block.bin            # 计算块级哈希
```

### 3.9 dryrun — 本地模拟迁移（无服务端）

```
client.exe dryrun [config.json]
```

在本地模拟完整的全量 + 增量迁移流程，**不需要服务端交互，不需要配置服务端地址**。所有网络 I/O 被 Mock，服务端 ACK 自动合成。适用于：

- 验证完整迁移流水线（读 → 哈希 → SQLite 去重 → 编码 → ACK）无需部署服务端
- 本地性能基准测试（读盘、哈希计算、SQLite 吞吐）
- 增量收敛逻辑调试

**运行流程**：

1. 读取配置文件（同真实迁移模式）
2. 枚举本地磁盘分区，打印磁盘信息
3. 初始化内部模块（queue、timer、SQLite），**跳过网络连接**
4. 执行 `do_migrate()` — 与真实迁移完全相同的流水线：
   - 打开磁盘读取器（VSS 快照优先，回退 PhysicalDrive）
   - 全量传输：逐块读取 → 计算哈希 → SQLite 去重 → 模拟发送 → 立即合成 ACK → 标记 `ack=1`
   - 18s 增量定时器：从 live disk 重读、哈希对比、重发变化块
   - TailSend=1 时进入增量收敛循环
5. 打印统计信息：耗时、模式、状态

**Mock 机制**：

| 函数 | 真实行为 | Dry-run 行为 |
|------|---------|-------------|
| `pool_init` | TCP 连接服务端 | 跳过 |
| `flush_one` | 出队 → wire_send → 释放连接 | 出队 → 立即合成 ACK → 标记 SQLite ack=1 |
| `process_ack` | 轮询所有连接接收 20B 响应 | 扫描队列中所有待确认条目 → 合成 ACK → 标记 ack=1 |
| `wire_send` | MsgPack → Zstd → "abc" → TCP 帧 → send() | 跳过（块数据仍完成 MsgPack 编码） |
| `wire_send_control` | 发送控制字符串（ctlIncremental 等） | 跳过 |
| `timer_cb_real` | 发送 ctlEndIncremental，等待 300s SERVER_DONE | 直接设置 allDone=1 |

**内部模块保持不变**：queue、timer、SQLite、hash、msgpack、volume、block_io、vss — 全部按真实模式运行。

**输出示例**：

```
Dry-run mode — no server connection needed
Disk   Size(GB) TotalBlocks      Name
------ ------ ---------------- ----
0      50.00    51200            C:
1      100.00   102400           D:

[INFO ] go2cloud client dry-run starting...
[INFO ] mode: full (zstd level 7, no network I/O)
[INFO ] partition C:: 51200 blocks via VSS
[INFO ] partition D:: 102400 blocks via PhysicalDrive
[INFO ] starting block transfer: 2 partitions
[INFO ] progress: partition 1/2 block 10000/51200 sent=10000/153600 queue=5
...
[INFO ] migration complete: 153600 blocks, 161061273600 bytes

========== Dry-Run Results ==========
  Mode:       full
  Status:     SUCCESS
  Duration:   42 seconds
======================================
```

**配置**：与真实迁移相同，使用 `user.json`：
- `TailSend: 1` → 全量 + 增量收敛循环
- `TailSend: 0` → 仅全量同步
- `DbPath` → 指定 SQLite 跟踪数据库路径（默认 `tracker.db`）

**权限**：需要管理员权限（与真实迁移一致，PhysicalDrive 访问要求）。

**退出码**：0 成功，1 失败（无磁盘可访问）。

---

## 4. 迁移模式

### 4.1 基本用法

```
client.exe 192.168.1.100:3389                  # 全量迁移（默认配置）
client.exe 192.168.1.100:3389 user.json        # 使用自定义配置
```

### 4.2 配置文件 (user.json)

```json
{
    "Log": {
        "Level":  3,
        "Path":   "client.log"
    },
    "TailSend":  0,
    "HasDump":   1,
    "SkipDisks": [3, 4],
    "Disks":     {"0": "\\\\.\\PhysicalDrive0"},
    "DbPath":    "tracker.db"
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `TailSend` | int | 0 | 1=增量模式（Zstd level 1），0=全量模式（Zstd level 7） |
| `HasDump` | int | 1 | 预留 |
| `SkipDisks` | int[] | [] | 要跳过的磁盘编号列表 |
| `Disks` | map | {} | 手动指定磁盘路径（覆盖自动枚举） |
| `DbPath` | string | "tracker.db" | SQLite 块跟踪数据库路径 |
| `Log.Level` | int | 3 (INFO) | 日志级别 |
| `Log.Path` | string | "" (stderr) | 日志文件路径 |

---

## 5. 迁移流程

### 5.1 完整流程图

```
                         ┌──────────────────────┐
                         │ 1. volume_enumerate   │  枚举所有固定磁盘分区
                         └──────────┬───────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 2. vss_init +         │  初始化 VSS
                         │    vss_create_snaps   │  创建持久快照
                         └──────────┬───────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 3. 打开读取器          │  VSS 快照路径优先,
                         │    (7 TCP 连接池)      │  回退到 PhysicalDrive
                         │    + live reader       │  TailSend 时额外打开
                         └──────────┬───────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 4. ctlIncremental     │  增量模式: 发送控制帧
                         │    (仅 TailSend=1)    │
                         └──────────┬───────────┘
                                    │
                  ┌─────────────────┴─────────────────┐
                  │  全量传输 + 18s 增量并发            │
                  │  ┌──────────────────────────┐      │
                  │  │ 4a. 逐块读取 VSS 快照     │      │
                  │  │     read → hash → SQLite  │      │
                  │  │     lookup → encode →     │      │
                  │  │     compress → send       │      │
                  │  └──────────────────────────┘      │
                  │  ┌──────────────────────────┐      │
                  │  │ 4b. 18s 定时器: 重读      │      │
                  │  │     live disk → hash 对比  │      │
                  │  │     → 重发变化块           │      │
                  │  └──────────────────────────┘      │
                  └─────────────────┬─────────────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 5. 排空队列            │  drain send queue
                         └──────────┬───────────┘
                                    │
                  ┌─────────────────┴─────────────────┐
                  │  TailSend=1: 增量收敛循环           │
                  │  ┌──────────────────────────┐      │
                  │  │ 6a. 每轮 18s:             │      │
                  │  │     - 重置 ACK=1 → ACK=0  │      │
                  │  │     - 查询未确认块         │      │
                  │  │     - Fisher-Yates 洗牌    │      │
                  │  │     - 读 live disk + hash  │      │
                  │  │     - hash 不同 → 重发     │      │
                  │  │     - hash 相同 → 标记稳定 │      │
                  │  └──────────────────────────┘      │
                  │  ┌──────────────────────────┐      │
                  │  │ 6b. should_finish 判定:   │      │
                  │  │     - 完美收敛 (ack=0)    │      │
                  │  │     - 稳定收敛 (N 轮不变) │      │
                  │  │     - 轮次上限 (60 轮)    │      │
                  │  └──────────────────────────┘      │
                  └─────────────────┬─────────────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 7. ctlEndIncremental  │  发送结束控制帧
                         │    等待 SERVER_DONE   │  服务端 fsync 完成
                         └──────────┬───────────┘
                                    │
                                    ▼
                         ┌──────────────────────┐
                         │ 8. 清理               │  BackupComplete,
                         │    DELETE FROM        │  关闭读取器,
                         │    T_BLOCK + exit(0)  │  释放 VSS
                         └──────────────────────┘
```

### 5.2 数据面发送流程（单个块）

```
   disk read (VSS 或 PhysicalDrive)
        │
        ▼
   hash_block() — 自定义 64-bit 哈希
        │
        ▼
   ┌─ SQLite 去重查询 ─────────────────────────────────┐
   │  SELECT hash FROM T_BLOCK WHERE devno=? AND offset=? │
   │                                                      │
   │  hash 相同 → 跳过 (块未变)                           │
   │  hash 不同 / 不存在 → 发送                           │
   └──────────────────────────────────────────────────────┘
        │ (需要发送)
        ▼
   MsgPack 编码: fixmap(3){devno, offset, data}
        │
        ▼
   Zstd 压缩 (level 1~7)
        │
        ▼
   TCP 帧化: 4B BE len + "abc" + payload
        │
        ▼
   入队 → 异步发送 → 接收 ACK → sqlite_block_mark_acked()
```

### 5.3 增量同步机制

**核心思想**：迁移工具的首要目标是产生一个 **崩溃一致性**（crash-consistent）的目标副本 — 等价于源端在迁移完成时刻突然断电。目标端启动后 NTFS 日志回放修复文件系统不一致性。

#### 5.3.1 18s 定时器：双层增量

go2cloud 的增量同步比原始 go2tencentcloud 更加彻底，包括两个层次：

**层次 1 — 全量传输期间的并发增量** (与原始一致)：
- 18s 定时器与全量同步并发运行
- 从 SQLite 查询 `ack=0` 的块（已发送但未确认）
- 从 **live PhysicalDrive**（非 VSS 快照）重读块数据
- 重新计算哈希并与已发送值对比
- 如果 live disk 上的数据已变化 → 重新发送新数据
- 121s 冷却期：同一块在 121 秒内不会被重复重读

**层次 2 — 全量后的独立增量收敛循环** (go2cloud 增强)：
- 全量同步完成后，进入独立的增量收敛循环
- 每轮开始：`UPDATE T_BLOCK SET ack=0 WHERE ack=1` — 将所有已确认块重置为未确认
- 从 live disk 重读所有块，对比哈希
- 仅重发变化的块
- 收敛判定后退出（见 5.3.2）

#### 5.3.2 收敛控制

三种退出条件（满足任意其一即结束增量同步）：

| 条件 | 判定标准 | 说明 |
|------|---------|------|
| **完美收敛** | `unacked == 0` | 所有块已确认，无变化块 |
| **稳定收敛** | 连续 3 轮 `requeued == 0` | 系统停止产生新写入 |
| **轮次上限** | `inc_round >= 60` | 约 30 分钟超时强制结束 |

收敛后，剩余未确认块依赖目标端 NTFS 日志回放修复。

#### 5.3.3 Fisher-Yates 洗牌

每轮增量同步前，通过 Fisher-Yates 算法随机打乱块的处理顺序。这避免了因顺序 I/O 模式导致的磁盘缓存命中偏差，使负载更均匀。

#### 5.3.4 121s 冷却期

每块在发送后进入 121 秒冷却期（`RETRANSMIT_MIN_INTERVAL_SEC`），冷却期内不会被重新读取和发送。这避免了对高频写入的热块进行过于频繁的重复传输。

冷却期通过 `T_BLOCK.last_sent` 列（毫秒时间戳）实现。

---

## 6. T_BLOCK 块跟踪数据库

### 6.1 表结构

```sql
CREATE TABLE IF NOT EXISTS T_BLOCK(
    devno     INTEGER,          -- 磁盘编号
    offset    INTEGER,          -- 块在磁盘上的字节偏移
    size      INTEGER,          -- 块大小 (通常 1MB)
    hash      INTEGER,          -- 块内容 64-bit 哈希
    ack       INTEGER DEFAULT 0, -- 0=未确认, 1=服务端已确认
    last_sent INTEGER DEFAULT 0, -- 上次发送的毫秒时间戳
    remote_id TEXT               -- 远程主机标识 (ip:port)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_block ON T_BLOCK(devno, offset);
CREATE INDEX IF NOT EXISTS idx_ack ON T_BLOCK(ack);
```

### 6.2 关键操作

| 操作 | SQL / 函数 | 说明 |
|------|-----------|------|
| 去重查询 | `sqlite_block_lookup(devno, offset, &hash)` | 检查块是否已发送过 |
| ACK 检查 | `sqlite_block_acked(devno, offset)` | 检查块是否已被服务端确认 |
| 插入/更新 | `sqlite_block_upsert(...)` | `INSERT OR REPLACE` — 通过唯一索引实现 upsert |
| 标记已确认 | `sqlite_block_mark_acked(devno, offset)` | 服务端 ACK 后调用，设 `ack=1` |
| 查询未确认 | `sqlite_get_unacked_with_hash(...)` | 增量同步输入：获取所有 `ack=0` 的块及其哈希和发送时间 |
| 更新发送时间 | `sqlite_update_last_sent(devno, offset, ts)` | 发送后更新 `last_sent` 用于冷却期判定 |
| 统计未确认 | `sqlite_count_unacked(db, remote_id)` | 收敛判定：统计 `ack=0` 块数 |
| 重置已确认 | `sqlite_reset_acked(db, remote_id)` | 增量轮开始：将所有 `ack=1` 重置为 `ack=0` |
| 查询已确认字节 | `sqlite_total_acked_bytes(db)` | `sentbytes` 子命令：跨进程进度查询 |
| 清空表 | `sqlite_clear_all_blocks(db)` | `end_session` / 迁移完成后清理 |

### 6.3 ACK 语义与生命周期

```
INSERT (ack=0, last_sent=ts)     ← 块首次发送
      │
      ▼
SELECT WHERE ack=0               ← 增量轮次查询未确认块
      │
      ├─ hash 未变 → UPDATE ack=1   ← 标记稳定（仅增量轮次中发生）
      │
      ├─ hash 变化 → 重新发送       ← ack 保持 0，等待 ACK
      │              │
      │              ▼
      │         服务端 ACK → UPDATE ack=1   ← 服务端确认
      │
      ▼
sqlite_reset_acked()              ← 下一增量轮次开始: 所有 ack=1 → ack=0
```

**关键设计**：TailSend 模式下，服务端 ACK 不直接标记 `ack=1`（仅在增量轮次的哈希匹配时才标记）。这确保了增量轮次有机会重读每个块并与 live disk 对比。全量（非 TailSend）模式下，服务端 ACK 正常标记 `ack=1`。

---

## 7. 工具架构

```
Go 控制端（go2tencentcloud）
│
├─ client.exe info              → 收集磁盘信息
├─ client.exe isbios            → 确定固件类型
├─ client.exe test_vss          → 创建持久快照（验证 VSS 可用性）
├─ client.exe begin_session     → 标记开始
│
├─ CreateProcess(               → 启动数据面传输
│     "client.exe <cloud_ip>:<port> user.json")
│
├─ 轮询: client.exe sentbytes   → 获取进度
│
├─ client.exe end_session       → 清理块跟踪数据库
│
├─ client.exe vss_query         → 查询现有快照
│
├─ client.exe vss_delete <guid> → 删除迁移快照
│
└─ client.exe vss_delete --all  → 清理所有快照
│
└─ client.exe dryrun [config]   → 本地模拟迁移（验证流水线 / 性能基准）
```

---

## 8. 与原始 go2tencentcloud 的关键差异

基于 Ghidra 反编译分析的对比：

### 8.1 数据结构差异

| 方面 | 原始 go2tencentcloud | 本实现 (go2cloud) |
|------|---------------------|-------------------|
| T_BLOCK 主键 | `ID INTEGER PRIMARY KEY AUTOINCREMENT` | 无独立 ID 列 |
| 唯一约束 | 无（`S1 INDEX` 非唯一，允许多行同 devno+offset） | `UNIQUE INDEX ON (devno, offset)` |
| 插入方式 | `INSERT INTO T_BLOCK`（追加新行） | `INSERT OR REPLACE`（覆盖旧行） |
| HASH 列类型 | `TEXT`（哈希字符串） | `INTEGER`（64-bit 整数） |
| 内存跟踪 | **红黑树** (`DAT_14022d178`) — 独立于 SQLite | 无 — 完全依赖 SQLite |
| 冷却期实现 | 红黑树节点 `last_sent` 字段 | SQLite `T_BLOCK.last_sent` 列 |

### 8.2 增量同步差异

| 方面 | 原始 | 本实现 |
|------|------|--------|
| 18s 定时器时机 | **仅并发于全量同步期间** | 全量期间并发 + 全量后独立收敛循环 |
| 未确认块来源 | `SELECT ... WHERE ACK=0` | 同 + 每轮 `sqlite_reset_acked()` 重置 |
| 哈希对比后 | 不变 → 跳过（ACK 保持 0）; 变 → 重发（ACK 保持 0） | 不变 → `mark_acked()`（设 ack=1）; 变 → 重发 |
| 收敛检测 | `timer_cb` 轮询 `SUM(SIZE) WHERE ACK=0`，为 0 则结束 | 3 种退出条件（完美/稳定/超时） |
| 完成后清理 | `DELETE FROM T_BLOCK WHERE REMOTE_ID!='%s'` + `exit(0)` | `sqlite_clear_all_blocks()` |

### 8.3 原始架构的关键设计点

1. **追加式 T_BLOCK**：原始没有唯一约束，同一个 (devno, offset) 可以有多行（不同 ACK 状态、不同哈希）。INSERT 不会覆盖旧数据，UPDATE 用精确的 WHERE 条件只更新匹配的行。

2. **双轨跟踪**：原始同时维护 SQLite（持久化）和红黑树（运行时）。红黑树用于：
   - 121s 冷却期查询（O(log n) 而非 SQL 查询）
   - ACK 处理：从树中移除已确认块
   - 计数：查询未确认块数量

3. **并发增量模型**：原始的 18s 定时器在全量同步期间并发运行，处理"已发送但未确认"的块。它没有全量后的独立增量阶段。当 `SUM(SIZE) WHERE ACK=0 == 0` 时立即结束。

4. **崩溃一致性**：两者都产生崩溃一致性副本。原始依赖 VSS 快照 + 并发增量捕获飞行中的变化；本实现在此基础上增加了全量后的收敛循环，更彻底但耗时更长。

---

## 9. 背压控制

防止发送过快导致服务端积压：

| 条件 | 动作 |
|------|------|
| 队列深度 > 40 (`BACKPRESSURE_QUEUE_DEPTH`) | `Sleep(50ms)` |
| 内存使用 > 45MB (`BACKPRESSURE_MEMORY_MB`) | `Sleep(50ms)` |
| 增量同步队列深度 > 9 | 暂停本轮，剩余块留待下轮 |
| 所有连接忙 | 等待连接释放 |
| 无可用 socket | `Sleep(1000ms)` |

---

## 10. 定时器

| 定时器 | 间隔 | 首次触发 | 职责 |
|--------|------|---------|------|
| `TIMER_RETRANSMIT` | 18s | 立即 | 重传未确认块 |
| `TIMER_INCREMENTAL` | 18s | 延迟 18s | 增量同步：重读 live disk，对比哈希 |
| `TIMER_RECONNECT` | 20s / 18s | 立即 | 检测过期连接，重建 |
| `TIMER_ACTION` | 10s | 延迟 5s | 进度日志输出 |

优先级：`RECONNECT > RETRANSMIT > ACTION > INCREMENTAL`

---

## 11. 编译

### 11.1 Windows (MSVC)

```bat
cl /O2 /utf-8 /Fe:client.exe ^
  client\block_io.c client\hash.c client\log.c client\main.c ^
  client\msgpack.c client\pool.c client\queue.c client\sqlite.c ^
  client\timer.c client\volume.c client\wire.c client\vss.c ^
  /Iinclude ^
  /link zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
```

**依赖库** (vcpkg):
- `zstd` — Zstd 压缩
- `sqlite3` — 块跟踪数据库
- `vssapi` + `ole32` — VSS 快照（仅 Windows）
- `ws2_32` — Winsock

### 11.2 Linux (桩实现)

```sh
gcc -O2 -o client client/*.c -Iinclude -lzstd -lsqlite3 -lpthread
```

> Linux 下 VSS 不可用，分区枚举为桩实现，需通过配置文件指定磁盘路径。

---

## 12. 运行时要求

| 要求 | 说明 |
|------|------|
| Windows Server 2012+ / Windows 10 Pro+ | VSS 需要专业版以上 |
| 管理员权限 | 物理磁盘访问 + VSS COM 调用 |
| vssapi.dll | 系统自带，无需安装 |
| Visual C++ 运行库 | 链接 `/MT` 可静态嵌入 |
| zstd.dll / sqlite3.dll | 与 client.exe 同目录 |
