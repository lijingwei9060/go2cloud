# go2cloud v2.0 增量迁移使用手册

## 1. v2.0 新特性：DCBT 内核驱动增量追踪

v1.x 的增量同步需要**重读全盘所有块、逐个计算哈希、与数据库对比**，时间复杂度和全量迁移一样（200 GB 磁盘 ≈ 204,800 次读盘 + 哈希计算）。

v2.0 引入 **DCBT（Driver Change Block Tracking）** 内核过滤驱动，在 Windows 内核层实时追踪磁盘写入。增量同步时只需查询驱动维护的脏块位图，**只读取和发送实际变化的块**，把增量 I/O 从 O(全盘) 降到 O(变化量)。

```
v1.x 增量:  读全盘 → 哈希对比 → 发送变化块      (200 GB 磁盘耗时 ~30 分钟)
v2.0 增量:  查询位图 → 只读脏块 → 发送           (变化 500 MB 耗时 ~5 秒)
```

## 2. 原理

```
┌─────────────────────────────────────────────────────┐
│  用户态 (client.exe)                                  │
│                                                       │
│  dcbt_get_bitmap(disk_N)  →  读取脏块位图             │
│  for each dirty block:                                │
│      read live disk  →  hash  →  send                 │
│  dcbt_clear_bitmap(disk_N) → 本轮完成后清零           │
│                                                       │
├─────────────────────────────────────────────────────┤
│  内核态 (go2cloud_flt.sys)                            │
│                                                       │
│  DiskDrive Class Upper Filter                        │
│    拦截所有 IRP_MJ_WRITE                              │
│    →  位图[offset / 1MB] = 1                          │
│                                                       │
│  位图 (NonPagedPool)                                  │
│    1 MB 粒度, 200 GB 磁盘 ≈ 25 KB                    │
└─────────────────────────────────────────────────────┘
```

**如果驱动未加载或不可用，client.exe 自动回退到 v1.x 的全量哈希对比模式**，不会出错。

## 3. 准备工作

### 3.1 开启测试签名模式

64 位 Windows 默认只加载有 EV 证书签名的驱动。开发/测试阶段需要开启测试签名：

```powershell
:: 在管理权限的 powershell 里面执行，确保返回 False（Secure Boot 必须关闭，否则 testsigning 不生效）
Confirm-SecureBootUEFI
```

```bat
:: 以管理员身份运行 cmd
bcdedit /set testsigning on
```

**重启后生效**。桌面右下角出现 "Test Mode" 水印表示已开启。

> **注意**：如果 `Confirm-SecureBootUEFI` 返回 `True`，必须进入 UEFI/BIOS 设置关闭 Secure Boot，否则即使 `testsigning on`，测试签名的驱动也无法加载。

### 3.2 构建驱动

```bat
cd driver
build_driver.bat
```

脚本自动完成：
1. 编译 4 个 `.c` 源文件（`/kernel /O2`）
2. 链接 `go2cloud_flt.sys`（KMDF 1.35）
3. 创建自签测试证书（首次运行）+ 签名 `.sys`
4. 通过 `inf2cat` 生成 `.cat` 目录文件 + 签名
5. 安装证书到受信根存储（需管理员权限）
6. 部署 `.sys`、`.inf`、`.cat` 到 `d:\migrate\cert\`

**注意**：步骤 4 需要 WDK 中的 `inf2cat.exe`（位于 `Windows Kits\10\bin\<version>\x86`）。

### 3.3 安装驱动

```bat
:: 推荐方式：pnputil 安装（需已签名的 .cat 目录文件）
pnputil /add-driver d:\migrate\cert\go2cloud_flt.inf /install

:: 备选方式：手动安装 PowerShell 脚本（绕过 pnputil，无需 .cat）
powershell -ExecutionPolicy Bypass -File d:\migrate\cert\install_driver.ps1
```

**`pnputil /add-driver` 要求驱动程序包包含经过签名的 `.cat` 目录文件。** `build_driver.bat` 会自动生成并签名 `.cat`。如果 `inf2cat.exe`（WDK 组件）不可用，请使用手动安装脚本 `install_driver.ps1`，该脚本会直接将驱动程序复制到 `System32\drivers`、创建服务并注册 UpperFilter。

驱动注册为 DiskDrive 类的 UpperFilter，安装后**需要重启**才会生效。

### 3.4 验证驱动已加载

安装 **DebugView**（Sysinternals 工具），以管理员运行，勾选 `Capture` → `Capture Kernel`。重启后应看到：

```
go2cloud_flt: loaded v2.0.0
go2cloud_flt: Disk0 ready — 204800 blocks, 25600 byte bitmap
go2cloud_flt: Disk1 ready — 102400 blocks, 12800 byte bitmap
```

或用 PowerShell 确认：

```powershell
driverquery /v | findstr go2cloud
```

### 3.5 验证 IOCTL 可用

```powershell
# 如果控制设备存在，说明驱动正常
# client.exe 会在增量同步时自动检测
```

## 4. 增量迁移工作流

### 4.1 典型流程

```
                    ┌─────────────────────┐
                    │ 1. 全量迁移           │
                    │    client.exe ip:port │
                    │    user.json          │
                    │    (TailSend: 0)      │
                    └──────────┬──────────┘
                               │ 完成后源端继续运行业务
                               ▼
                    ┌─────────────────────┐
                    │ 2. 第一次增量         │
                    │    client.exe incsync │
                    │    ip:port user.json  │
                    │    → 只传变化块       │
                    └──────────┬──────────┘
                               │ 可重复多次
                               ▼
                    ┌─────────────────────┐
                    │ 3. 第 N 次增量        │
                    │    client.exe incsync │
                    │    ip:port user.json  │
                    │    → 变化越少越快     │
                    └──────────┬──────────┘
                               │ 准备割接，停业务
                               ▼
                    ┌─────────────────────┐
                    │ 4. 最终增量 + 割接    │
                    │    client.exe incsync │
                    │    ip:port user.json  │
                    │    → 最后一轮同步     │
                    └─────────────────────┘
```

### 4.2 第一步：全量迁移

```bat
client.exe 192.168.1.100:3389 user.json
```

**配置文件 `user.json`**：

```json
{
    "Log": {"Level": 2, "Path": "client.log"},
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

全量迁移读取 VSS 快照，逐块哈希、压缩、发送。完成后 `tracker.db` 中记录了所有块的哈希值，供后续增量对比使用。

### 4.3 第二步及以后：增量同步

```bat
client.exe incsync 192.168.1.100:3389 user.json
```

**`incsync` 子命令的 DCBT 加速流程：**

```
1. 连接服务端，发送 ctlIncremental
2. 对每个磁盘：
   a. dcbt_get_bitmap(disk_N)      →  获取脏块位图（IOCTL）
   b. 遍历位图中标记为脏的块：
      - 读 live disk → 计算哈希 → 与 tracker.db 对比
      - 哈希不同 → MsgPack 编码 → 压缩 → 发送
      - 哈希相同 → 标记已验证（更新 version）
   c. dcbt_clear_bitmap(disk_N)    →  清零位图（IOCTL）
3. 排空发送队列
4. 发送 ctlEndIncremental，等待服务端确认
5. 打印统计：版本号、扫描数、变化数、发送字节数
```

**关键点：**
- 只遍历位图中标记为脏的块，不扫描全盘
- 每轮自动获取新版本号（`T_VERSION.version` 自增）
- 如果驱动不可用，自动回退到全量哈希对比（v1.x 行为）

### 4.4 输出示例

```
Partitions to sync:
Idx  Disk     Part#      Name             Skip
---- ---- -------- ---------------- ----
0    0        1         C:               sync
1    1        1         D:               sync

[INFO ] incsync: DCBT driver available — using dirty bitmap mode
[INFO ] incsync: Disk0 — 204800 blocks, dirty=42 (0.02%)
[INFO ] incsync: Disk0 — scanned 42, changed 15, skipped 27
[INFO ] incsync: Disk1 — 102400 blocks, dirty=8 (0.01%)
[INFO ] incsync: Disk1 — scanned 8, changed 3, skipped 5
[INFO ] incsync: draining queue (2 pending)...

========== IncSync Results ==========
  Version:     3
  DCBT:        enabled (dirty bitmap mode)
  Total dirty: 50 blocks (0.02% of all tracked)
  Scanned:     50 blocks
  Unchanged:   32 (hash matched, marked verified)
  Changed:     18 blocks
  Sent:        18874368 bytes (0.02 GB)
  Duration:    3 seconds
======================================
```

对比没有 DCBT 时（v1.x 回退模式）的输出：

```
========== IncSync Results ==========
  Version:     3
  DCBT:        disabled (full hash comparison mode)
  Scanned:     307200 blocks    ← 全盘扫描
  Unchanged:   307182
  Changed:     18 blocks
  Sent:        18874368 bytes
  Duration:    1847 seconds     ← 30 分钟
======================================
```

## 5. 配置参考

### 5.1 user.json 完整字段

```json
{
    "Log": {
        "Level": 2,
        "Path": "client.log"
    },
    "TailSend": 0,
    "HasDump": 1,
    "DbPath": "tracker.db",
    "SkipDisks": [3, 4],
    "SkipPartitions": "0:2,0:3,1:1",
    "Disks": {
        "0": "\\\\.\\PhysicalDrive0",
        "1": "\\\\.\\PhysicalDrive1"
    }
}
```

| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `TailSend` | int | 0 | 全量同步用 0；incsync 自动忽略此字段 |
| `DbPath` | string | `tracker.db` | 块跟踪数据库，incsync 复用全量同步的 DB |
| `SkipDisks` | int[] | [] | 要跳过的磁盘编号 |
| `SkipPartitions` | string | "" | 跳过分区，格式 `"devno:part,devno:part,..."` |
| `Disks` | map | {} | 手动指定磁盘路径（覆盖自动枚举） |

### 5.2 日志级别

| Level | 含义 |
|-------|------|
| 0 | ERROR |
| 1 | WARN |
| 2 | INFO |
| 3 | DEBUG |

## 6. 版本历史追踪

每次 `incsync` 自动在 T_VERSION 表中记录一轮信息：

```bash
client.exe blockinfo --history
```

输出：

```
Version History:
Ver   Start Time           End Time             Scanned  Changed  Duration
----- -------------------- -------------------- -------- -------- ----------
1     2026-07-01 10:00:00  2026-07-01 10:30:00  153600   153600   1800s
2     2026-07-02 09:00:00  2026-07-02 09:00:05  42       15       5s
3     2026-07-02 09:15:00  2026-07-02 09:15:03  8        3        3s
```

- Version 1：全量同步
- Version 2+：增量轮次

## 7. 故障排查

### 7.1 驱动未加载

**现象**：incsync 日志显示 `DCBT: disabled (full hash comparison mode)`

**排查**：
```bat
:: 检查驱动是否安装
driverquery /v | findstr go2cloud

:: 检查 testsigning 是否开启
bcdedit /enum | findstr testsigning
:: 应输出: testsigning           Yes

:: 检查 DebugView 是否有驱动输出（Capture Kernel 开启）
```

**常见原因**：
- testsigning 未开启 → `bcdedit /set testsigning on` + 重启
- 驱动未签名或证书未受信 → 以管理员重跑 `build_driver.bat`
- 驱动安装后未重启

### 7.2 控制设备打不开

**现象**：DebugView 无 "DiskN ready" 输出

**可能原因**：
- 系统没有固定磁盘（仅移动介质）
- 驱动加载了但 PrepareHardware 未触发 → 检查设备管理器中的磁盘是否正常

### 7.3 蓝屏（BSOD）

**恢复步骤**：
1. 开机按 F8 → 安全模式
2. 删除 `C:\Windows\System32\drivers\go2cloud_flt.sys`
3. 或以管理员运行：`sc delete go2cloud_flt`
4. 重启

**排查**：分析 `C:\Windows\Minidump\` 中的 `.dmp` 文件（WinDbg）。

### 7.4 增量同步仍然很慢

**可能原因**：
- DCBT 驱动未加载（回退到全量哈希模式）
- 两次 incsync 之间写入量很大（位图中脏块多）
- 配置中 SkipDisks/SkipPartitions 未正确设置，包含了不需要同步的磁盘

### 7.5 签名时间戳服务不可达

`build_driver.bat` 签名步骤不使用时间戳服务器。自签证书有效期 1 年，过期后重新运行 `build_driver.bat` 即可（会创建新证书并重新签名）。

## 8. 卸载驱动

```bat
:: 查询驱动 INF
pnputil /enum-drivers | findstr go2cloud

:: 卸载（使用上面查到的 published name，例如 oemXX.inf）
pnputil /delete-driver oemXX.inf /uninstall

:: 关闭测试签名（如不再需要）
bcdedit /set testsigning off
```

卸载后重启生效。

## 9. 快速参考

```bat
:: === 一次性环境准备 ===
bcdedit /set testsigning on                        # 开启测试签名
:: 重启
cd driver && build_driver.bat                      # 编译 + 签名 + 部署到 d:\migrate\cert
pnputil /add-driver d:\migrate\cert\go2cloud_flt.inf /install   # 安装驱动
:: 如果 pnputil 失败，使用备选方案：
:: powershell -ExecutionPolicy Bypass -File d:\migrate\cert\install_driver.ps1
:: 重启

:: === 验证驱动 ===
:: 打开 DebugView，勾选 Capture Kernel
driverquery /v | findstr go2cloud                  # 应看到 go2cloud_flt

:: === 迁移流程 ===
client.exe 192.168.1.100:3389 user.json            # 全量迁移（TailSend: 0）
:: ... 源端继续运行业务（数小时/数天）...
client.exe incsync 192.168.1.100:3389 user.json    # 增量同步 1
client.exe incsync 192.168.1.100:3389 user.json    # 增量同步 2
:: ... 准备割接，停止源端业务 ...
client.exe incsync 192.168.1.100:3389 user.json    # 最终增量
:: 割接到目标端
```

## 10. 生产环境部署

生产环境需要 EV 代码签名证书（DigiCert / GlobalSign，约 $300-500/年）：

```bat
:: 用 EV 证书签名
signtool sign /v /fd sha256 /f ev_cert.pfx /p <password> /tr http://timestamp.digicert.com /td sha256 go2cloud_flt.sys

:: 提交 Microsoft 认证签名（可选，使驱动在 Secure Boot 开启时也能加载）
:: 通过 Windows Hardware Dev Center 提交 .cab 文件
```

EV 签名驱动在所有机器上均可加载，**无需开启 testsigning**。
