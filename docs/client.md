# go2cloud 客户端 (client.exe) 使用文档

## 1. 概述

`client.exe` 是 go2cloud 块级迁移工具的数据面程序，负责：

- 枚举源端物理磁盘分区
- 通过 VSS 创建一致性快照
- 逐块读取、哈希、压缩、发送磁盘数据
- 接收 ACK 确认，支持崩溃恢复
- 通过子命令向 Go 控制端提供磁盘/固件/进度信息

`client.exe` 可被 **Go 控制端**（通过 `CreateProcess`）或 **用户直接**调用。

## 2. 命令行模式

### 2.1 子命令模式（信息采集 / 会话控制）

```
client.exe info                  # 显示磁盘分区信息
client.exe hash <file>           # 计算文件哈希
client.exe check <disk>          # 检查磁盘可访问性
client.exe isbios                # 检测固件类型
client.exe begin_session         # 标记迁移会话开始
client.exe end_session           # 清理块跟踪数据库
client.exe sentbytes             # 查询已确认总字节数
client.exe test_vss              # 测试 VSS 快照功能（创建持久快照）
client.exe vss_query              # 查询所有现有快照
client.exe vss_delete <guid>       # 删除指定快照
client.exe vss_delete --all        # 删除所有快照
client.exe --help                # 显示帮助
```

### 2.2 迁移模式（数据面传输）

```
client.exe <server_ip:port> [config.json]
```

## 3. 子命令详解

### 3.1 info — 分区信息

```
client.exe info
```

枚举所有固定磁盘上的分区，按盘符（如果有）和组织名称显示。输出格式：

```
Disk Information:
DevNo  Size(GB) TotalBlocks      Name             DiskPath
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
4. `vss_cleanup()` — 进程退出，快照保留

### 3.4 vss_query — 查询快照

```
client.exe vss_query
```

列出系统上所有 `ClientAccessible` 类型的持久快照。输出格式：

```
Shadow Copy ID: {a8eda6c5-8d6d-43ff-a022-25010767481a}
  Original Volume: \\?\Volume{7f6da094-ef53-45fb-a850-7380ba52e6d4}\
  Device Object:   \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy5
  Creation Time:   2026-06-30 13:56:21
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

## 4. 迁移模式

### 4.1 基本用法

```
client.exe 192.168.1.100:3389                  # 全量迁移（默认配置）
client.exe 192.168.1.100:3389 user.json        # 使用自定义配置
client.exe 192.168.1.100:3389 user.json incremental  # 增量模式（已弃用，使用 TailSend 配置）
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

### 4.3 迁移流程

```
                           ┌──────────────────────┐
                           │ 1. volume_enumerate  │  枚举所有固定磁盘分区
                           └──────────┬───────────┘
                                      │
                                      ▼
                           ┌──────────────────────┐
                           │ 2. vss_init_ex       │  初始化 VSS (持久快照)
                           │    (0x1d)            │  ClientAccessible 上下文
                           │    vss_create_snap   │  为带盘符的卷创建快照
                           └──────────┬───────────┘
                                      │
                                      ▼
                           ┌──────────────────────┐
                           │ 3. 打开读取器         │  VSS 快照路径优先,
                           │    (7 TCP 连接池)     │  回退到 PhysicalDrive
                           └──────────┬───────────┘
                                      │
                                      ▼
                           ┌──────────────────────┐
                           │ 4. 块传输循环         │  逐分区 × 逐块:
                           │    read → hash →      │  · 读块 (VSS 相对偏移)
                           │    lookup → encode →  │  · 去重查询 SQLite
                           │    compress → send    │  · Zstd + MsgPack + TCP
                           └──────────┬───────────┘
                                      │
                                      ▼
                           ┌──────────────────────┐
                           │ 5. 排空 → 清理       │  queue drain,
                           │    vss_cleanup        │  BackupComplete,
                           │    close              │  关闭读取器
                           └──────────────────────┘
```

### 4.4 增量模式

增量模式（`TailSend=1`）在首次全量传输完成后启用：

1. 发送 `ctlIncremental` 控制帧
2. 全量传输所有块
3. 对每个块：计算 hash → 查询 `T_BLOCK` 中上次存储的 hash → 相同则跳过，不同则发送
4. 发送 `ctlEndIncremental`，等待服务端 `SERVER_DONE`
5. 下一轮增量：`tail send enabled` 从上次断点继续

### 4.5 崩溃恢复

客户端支持崩溃后断点续传：
- 每个 ACK 立即写入 SQLite（`T_BLOCK SET ack=1`）
- 重启后 `send_block()` 跳过已 ACK 的块
- 无需全量重传

### 4.6 背压控制

防止发送过快导致服务端积压：

| 条件 | 动作 |
|------|------|
| 队列深度 > 40 | `Sleep(50ms)` |
| 内存使用 > 45MB | `Sleep(50ms)` |
| 所有连接忙 | 等待连接释放 |

## 5. 工具架构

```
Go 控制端（go2tencentcloud）
│
├─ client.exe info              → 收集磁盘信息
├─ client.exe isbios            → 确定固件类型
├─ client.exe test_vss          → 创建持久快照（验证 VSS 可用性）
├─ client.exe begin_session     → 标记开始
│
├─ CreateProcess(               → 启动数据面传输
│     "client.exe <cloud_ip> <port>")
│
├─ 轮询: client.exe sentbytes   → 获取进度
│
├─ client.exe end_session       → 清理块跟踪数据库
│
├─ client.exe vss_query         → 查询现有快照
│
├─ client.exe vss_delete <guid>  → 删除迁移快照
│
└─ client.exe vss_delete --all   → 清理所有快照
```

## 6. 编译

### 6.1 Windows (MSVC)

```bat
cl /O2 /utf-8 /c /Tpclient\vss.c /Iinclude /Fovss.obj

cl /O2 /utf-8 /Fe:client.exe ^
  client\block_io.c client\hash.c client\log.c client\main.c ^
  client\msgpack.c client\pool.c client\queue.c client\sqlite.c ^
  client\timer.c client\volume.c client\wire.c vss.obj ^
  /Iinclude ^
  /link zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
```

**依赖库** (vcpkg):
- `zstd` — Zstd 压缩
- `sqlite3` — 块跟踪数据库
- `vssapi` + `ole32` — VSS 快照（仅 Windows）
- `ws2_32` — Winsock

### 6.2 Linux (桩实现)

```sh
gcc -O2 -o client client/*.c -Iinclude -lzstd -lsqlite3 -lpthread
```

> Linux 下 VSS 不可用，分区枚举为桩实现，需通过配置文件指定磁盘路径。

## 7. 运行时要求

| 要求 | 说明 |
|------|------|
| Windows Server 2012+ / Windows 10 Pro+ | VSS 需要专业版以上 |
| 管理员权限 | 物理磁盘访问 + VSS COM 调用 |
| vssapi.dll | 系统自带，无需安装 |
| Visual C++ 运行库 | 链接 `/MT` 可静态嵌入 |
| zstd.dll / sqlite3.dll | 与 client.exe 同目录 |
