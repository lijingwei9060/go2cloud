# go2cloud 集成测试指南 — Windows 10 块级迁移

## 1. 测试拓扑

```
┌──────────────────────┐         TCP:3389         ┌──────────────────────┐
│   源端 (Source)       │ ──────────────────────→  │   目标端 (Target)     │
│   Windows 10         │   7 路并发连接            │   Windows / Linux    │
│                       │                          │                      │
│   client.exe          │   ←── 20B ACK 响应 ──    │   receiver.exe       │
│   读取源磁盘块         │                          │   写入目标磁盘/文件    │
│   VSS 快照 (可选)      │                          │                      │
└──────────────────────┘                          └──────────────────────┘
```

**网络要求**：源端能 TCP 连通目标端 3389 端口（或自定义端口），防火墙/安全组放行。

---

## 2. 环境准备

### 2.1 编译可执行文件

在开发机上执行：
```bat
cd D:\source\lijingwei9060\go2cloud
build_all.bat
```

产出：
- `receiver.exe` — 服务端（部署到目标端）
- `client.exe` — 客户端（部署到源端）

### 2.2 目标端（Receiver）准备

**存储空间**：目标端需要 ≥ 源端被迁移磁盘总大小的可用空间。

两种写入目标模式：
| 模式 | 配置示例 | 说明 |
|------|---------|------|
| 写入文件（推荐测试） | `"0": "D:\migrate\disk0.img"` | receiver 自动创建稀疏文件 |
| 写入物理磁盘 | `"0": "\\\\.\\PhysicalDrive2"` | 需要管理员权限，会覆盖目标磁盘 |

> **警告**：写入物理磁盘会**覆盖目标磁盘全部数据**。首次测试强烈建议使用文件模式。

**创建 receiver.json**（放在 receiver.exe 同目录）：

```json
{
  "Listen": {
    "Address": "0.0.0.0",
    "Port": 3389,
    "MaxConnections": 7
  },
  "Target": {
    "Disks": {
      "0": "D:\\migrate\\disk0.img"
    }
  },
  "Log": {
    "Level": 1,
    "Path": "receiver.log"
  }
}
```

### 2.3 源端（Client）准备

**权限要求**：客户端需要**管理员权限**运行（读取 `\\.\PhysicalDriveX` 需要）。

**创建 user.json**（放在 client.exe 同目录）：

```json
{
  "Log": {
    "Level": 1,
    "Path": "client.log"
  },
  "DbPath": "tracker.db",
  "TailSend": 0,
  "HasDump": 1,
  "SkipDisks": []
}
```

---

## 3. 测试步骤

### 阶段一：前置检查（源端）

#### 测试 1.1 — 查看帮助

```bat
client.exe --help
```

**预期**：打印用法说明，列出 `info`、`hash`、`check`、`<ip:port>` 四个模式。

#### 测试 1.2 — 枚举磁盘

```bat
client.exe info
```

**预期输出**：
```
Disk Information:
DevNo  Size(GB) TotalBlocks(1MB) Path
------ ------ ---------------- ----
0      256.00  262144           \\.\PhysicalDrive0
1      512.00  524288           \\.\PhysicalDrive1
...
```

确认能看到源端所有固定磁盘及其大小。

#### 测试 1.3 — 检查磁盘可访问性

```bat
client.exe check \\.\PhysicalDrive0
```

**预期输出**：
```
Disk: \\.\PhysicalDrive0
  Total size:   xxx.xx GB
  Total blocks: xxxxxx (@ 1MB)
  First block:  xxxxxx bytes, hash=0x...
  Status:       OK
```

`Status: OK` 表示可以正常读取块数据。如果显示 `READ ERROR`，检查是否以管理员权限运行。

#### 测试 1.4 — 哈希计算验证

```bat
echo HelloWorld > testdata.bin
client.exe hash testdata.bin
```

**预期**：输出文件大小和 64 位哈希值（十六进制）。

### 阶段二：全量迁移（端到端）

这是核心集成测试，将源端一块磁盘的全部数据迁移到目标端。

#### 步骤 2.1 — 启动 Receiver

在目标端机器上：

```bat
receiver.exe --config receiver.json
```

**预期日志输出**：
```
[INFO] go2cloud receiver starting...
[INFO] registered disk 0 -> D:\migrate\disk0.img
[INFO] opened target disk 0: D:\migrate\disk0.img (fd=3)
[INFO] listening on 0.0.0.0:3389 (max 7 connections)
[INFO] Receiver ready, waiting for connections...
```

#### 步骤 2.2 — 启动 Client

在源端机器上（**以管理员身份运行 cmd**）：

```bat
client.exe <目标端IP>:3389 user.json
```

其中 `<目标端IP>` 替换为 receiver 所在机器的 IP 地址。

**预期行为**：
1. 客户端枚举磁盘 → 建立 7 路 TCP 连接 → 开始逐块读取和发送
2. 每 10 秒打印进度日志：
   ```
   [INFO] progress: disk 1/1 block 1024/262144 sent=1024/262144 queue=5
   ```
3. 如果队列超过 40 块，触发背压 → 自动等待排空

#### 步骤 2.3 — 观察 Receiver 日志

目标端应同步打印：
```
[DEBUG] wrote block devno=0 offset=0 size=1048576 (total: 1 blocks, 1048576 bytes)
[DEBUG] wrote block devno=0 offset=1048576 size=1048576 (total: 2 blocks, 2097152 bytes)
...
```

#### 步骤 2.4 — 等待完成

全量迁移时间估算：
```
时间 ≈ 磁盘总块数 × 1MB / (并发数 × 网络吞吐)

示例: 100GB 磁盘 = 102400 块
      千兆网络 ~100MB/s → 约 17 分钟
```

迁移完成后客户端打印：
```
[INFO] draining send queue...
[INFO] migration complete: 102400 blocks, 107374182400 bytes
[INFO] cleaning up...
```

#### 步骤 2.5 — 验证数据完整性

迁移完成后，在源端和目标端分别计算磁盘/文件哈希做比对：

**源端**（计算前 100 块的累积哈希）：
```bat
client.exe check \\.\PhysicalDrive0
```

**目标端**（可用 certutil 或自行比对）：
```bat
certutil -hashfile D:\migrate\disk0.img SHA256
```

> 对精确验证：可以在源端生成一个已知内容的测试文件，写入到源盘特定位置，迁移后在目标端相同偏移处验证内容一致。

### 阶段三：增量迁移

增量迁移在已有全量迁移的基础上，只传输变化的块。

#### 步骤 3.1 — 修改 user.json

```json
{
  "Log": { "Level": 1, "Path": "client.log" },
  "DbPath": "tracker.db",
  "TailSend": 1,
  "HasDump": 1,
  "SkipDisks": []
}
```

`"TailSend": 1` 告诉客户端进入增量模式（zstd level=1，查询 SQLite 去重）。

#### 步骤 3.2 — 重启 Receiver

先 `Ctrl+C` 停止 receiver，再重新启动（目标文件保留不要删除）。

#### 步骤 3.3 — 运行增量迁移

```bat
client.exe <目标端IP>:3389 user.json
```

**预期差异**：
1. 客户端发送 `ctlIncremental` 控制消息
2. 每个块先查 SQLite → hash 相同的块跳过（日志显示 `skip: devno=0 offset=xxx (hash unchanged)`）
3. 全量轮完成后发送 `ctlEndIncremental` → receiver 执行 `fsync` → 返回 `SERVER_DONE`
4. 增量轮仅传输变化的块，速度远快于全量

#### 步骤 3.4 — 验证

增量完成后检查 receiver 日志是否有：
```
[INFO] session X incremental mode enabled
[INFO] session X incremental round complete, fsyncing...
[INFO] session X incremental round finished
```

### 阶段四：异常场景

#### 测试 4.1 — Receiver 未启动

客户端在没有 receiver 监听时启动：
```bat
client.exe 127.0.0.1:3389 user.json
```

**预期**：`pool_init: cannot establish any connection to 127.0.0.1:3389`，客户端退出。

#### 测试 4.2 — 迁移中途 Receiver 重启

1. 正常启动全量迁移
2. 迁移进行到 ~10% 时 `Ctrl+C` 停止 receiver
3. 重启 receiver（保留目标文件）
4. 重启 client（tracker.db 保留）

**预期**：
- 连接池检测到断连 → 自动重连
- 重传定时器（18s）触发未 ACK 的块重传
- 已写入目标端的块在重传时被覆盖写入（幂等）

#### 测试 4.3 — 跳过指定磁盘

修改 user.json 中的 `SkipDisks`：
```json
{ "SkipDisks": [3, 4] }
```

**预期**：devno=3 和 devno=4 的磁盘被跳过，日志输出 `skipping disk 3 (in skip list)`。

#### 测试 4.4 — 磁盘路径重映射

修改 user.json 中的 `Disks`：
```json
{
  "Disks": {
    "0": "\\\\.\\PhysicalDrive2",
    "1": "\\\\.\\PhysicalDrive3"
  }
}
```

**预期**：源端 PhysicalDrive2 的块以 devno=0 发送，PhysicalDrive3 以 devno=1 发送。目标端按 devno 写入对应的目标磁盘/文件。

---

## 4. 测试用例清单

| 编号 | 测试项 | 类型 | 预期结果 |
|------|--------|------|---------|
| T1.1 | `client.exe --help` | 功能 | 打印帮助信息 |
| T1.2 | `client.exe info` | 功能 | 列出所有固定磁盘 |
| T1.3 | `client.exe check \\.\PhysicalDrive0` | 功能 | 磁盘可读，显示大小和首块哈希 |
| T1.4 | `client.exe hash <file>` | 功能 | 输出文件哈希 |
| T2.1 | 全量迁移 1 块磁盘 | 集成 | 源端全部块传输完成，目标端文件大小匹配 |
| T2.2 | 全量迁移多块磁盘 | 集成 | 所有磁盘按 devno 正确写入 |
| T2.3 | 数据完整性验证 | 验证 | 目标端文件内容与源端磁盘逐块一致 |
| T3.1 | 增量迁移（仅变化块） | 集成 | 跳过未变化块，仅传输变化块 |
| T3.2 | 增量迁移（全量无变化） | 集成 | 所有块被跳过，瞬间完成 |
| T4.1 | Receiver 未启动 | 异常 | 客户端连接失败并退出 |
| T4.2 | 迁移中途 Receiver 重启 | 韧性 | 自动重连 + 重传，最终一致 |
| T4.3 | SkipDisks 过滤 | 功能 | 指定磁盘被跳过 |
| T4.4 | 磁盘路径重映射 | 功能 | 按配置映射 devno |

---

## 5. 日志级别参考

| Level | 值 | 描述 |
|-------|---|------|
| TRACE | 0 | 最详细（包括每个块的编解码细节） |
| DEBUG | 1 | 调试（块发送、ACK 接收、哈希计算结果） |
| INFO  | 2 | 常规（进度、连接建立/关闭、迁移完成） |
| WARN  | 3 | 警告（背压触发、重连、跳过块） |
| ERROR | 4 | 错误（读写失败、连接断开） |

配置文件中的 `"Level": 1` 对应 DEBUG，调试时推荐使用。生产环境建议 `"Level": 2`（INFO）。

---

## 6. 常见问题

### Q: `client.exe info` 没有列出任何磁盘
**A**: 必须以管理员权限运行。右键 cmd → "以管理员身份运行"。

### Q: `pool_init: cannot establish any connection`
**A**: 检查目标端防火墙是否放行 3389 端口，receiver 是否已启动，IP 是否正确。

### Q: 迁移很慢（< 1MB/s）
**A**: 检查网络带宽。可调整 `SOCKET_POOL_TARGET`（protocol.h，默认 7）增加并发连接数。

### Q: `queue_push: payload too large`
**A**: 编码后的 MsgPack 消息超过 1MB+256B 上限。检查是否有异常大的块数据。

### Q: 目标端磁盘空间不足
**A**: 源端 `info` 显示的磁盘大小即为目标端需要的空间。提前准备足够空间。

### Q: 如何迁移系统盘（C: 盘）
**A**: `\\.\PhysicalDrive0` 通常就是系统盘。VSS 快照可以确保打开的文件和应用数据一致性，但 VSS 功能目前代码中已有框架，实际快照创建待集成。
