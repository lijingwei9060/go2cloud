# DCBT (Driver Change Block Tracking) — 2.0.0 设计文档

## 概述

DCBT 在内核态追踪磁盘写入，维护 per-disk 的脏块位图。增量同步时用户态直接查询该位图，
只读取和发送变化块，把增量同步的 I/O 从 **O(全盘)** 降到 **O(变化量)**。

### 核心架构

```
┌─────────────────────────────────────────────────┐
│  用户态 (client.exe)                              │
│                                                   │
│  client/dcbt.c                                    │
│    dcbt_open(disk_number)                         │
│    dcbt_get_bitmap(disk_number, buf, size)        │
│    dcbt_clear_bitmap(disk_number)                 │
│    dcbt_get_stats(disk_number, stats)             │
│                                                   │
│  client/main.c — 增量同步循环                      │
│    旧: for each unacked block → read + hash       │
│    新: bitmap = dcbt_get(N)                       │
│         for each dirty block → read + hash        │
│         dcbt_clear(N)                             │
│                                                   │
├─────────────────────────────────────────────────┤
│  内核态 (go2cloud_flt.sys)                       │
│                                                   │
│  Disk Class Upper Filter                          │
│    IRP_MJ_WRITE → MarkDirty(offset, length)       │
│    IRP_MJ_DEVICE_CONTROL → IOCTL dispatch         │
│                                                   │
│  位图 (NonPagedPool)                              │
│    200 GB 磁盘 → 25,600 bytes                     │
│    2 TB 磁盘   → 256 KB                           │
│                                                   │
└─────────────────────────────────────────────────┘
```

### 原理

go2aliyun 的 DCBT（smcss 驱动）和我们设计功能一致：
- 内核过滤驱动，挂在磁盘栈上拦截写操作
- 维护 per-disk 脏块位图
- 用户态通过 IOCTL 获取/清零位图
- 增量同步只处理位图中标记的块

go2cloud 当前做法是全量重读所有块并做哈希对比，时间复杂度和全量迁移一样。
DCBT 只在写入发生时更新位图（零 I/O 开销），查询时只返回变化的块。

---

## 实现路径评估

### 方案对比

| 方案 | 原理 | 精度 | 复杂度 | 签名需求 |
|------|------|------|--------|----------|
| **A. KMDF Upper Filter** | 挂在 DiskDrive 类上，拦截 IRP_MJ_WRITE | 1 MB 粒度 | 中 | EV 证书 / WHQL |
| **B. VSS Diff Area 查询** | 查询 volsnap.sys 内部 diff area 位图 | 扇区级 | 高(undocumented) | 无 |
| **C. NTFS USN Journal** | 读取 USN Journal 找文件变更→映射到簇→扇区 | 文件级 | 中 | 无 |
| **D. ETW DiskIO Trace** | 捕获 ETW DiskIO 事件，用户态维护位图 | 扇区级 | 低 | 无 |

**选择方案 A**，理由：
- 精度最高（1 MB 粒度，块级精确）
- I/O 开销为零（写操作路径上只做几个位运算）
- 可控性强（我们拥有全部代码）
- 生产环境需要 EV 签名，开发/测试可用 test signing
- 方案 B 依赖未文档化 API，跨版本不稳定
- 方案 C 需要 NTFS 解析 + 簇映射，碎片文件处理复杂
- 方案 D 事件可能丢失，高负载下不可靠

### KMDF 驱动详细设计

#### 过滤位置

```
HKLM\SYSTEM\CurrentControlSet\Control\Class\
  {4D36E967-E325-11CE-BFC1-08002BE10318}  ← DiskDrive 类 GUID
    UpperFilters = "go2cloud_flt"          ← 注册为 UpperFilter
```

挂载在 `\Driver\disk` → `\Device\Harddisk0\DR0` 上方。所有发往物理磁盘的
写 IRP 经过过滤驱动。IRP 中的 `Parameters.Write.ByteOffset` 是物理磁盘偏移，
和 go2cloud 的 `volume_info_t.partition_offset` 一致，可以直接对照。

#### 位图设计

```
位图粒度:  1 MB (BLOCK_SIZE = 0x100000)
位图大小:  (total_disk_bytes / BLOCK_SIZE + 7) / 8 bytes

典型值:
  200 GB → 204,800 blocks → 25,600 bytes (25 KB)
  500 GB → 512,000 blocks → 64,000 bytes (63 KB)
  2 TB   → 2,097,152 blocks → 262,144 bytes (256 KB)
```

位图保存在驱动的 Device Extension (NonPagedPool) 中。
驱动加载时分配，卸载时释放。

#### IOCTL 接口

```c
#define GO2CLOUD_FLT_DEVICE_TYPE  FILE_DEVICE_UNKNOWN

// 获取脏块位图
#define IOCTL_GO2CLOUD_GET_BITMAP \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

// 清零位图
#define IOCTL_GO2CLOUD_CLEAR_BITMAP \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

// 获取统计信息
#define IOCTL_GO2CLOUD_GET_STATS \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

// 位图传输结构
typedef struct _GO2CLOUD_BITMAP {
    ULONG   DiskNumber;      // PhysicalDriveN
    ULONG64 TotalBytes;      // 磁盘总字节数
    ULONG   BlockSize;       // 1 MB
    ULONG   TotalBlocks;     // 总块数
    ULONG   DirtyBlocks;     // 当前脏块数
    ULONG   BitmapSize;      // 位图字节数 = (TotalBlocks+7)/8
    UCHAR   Bitmap[1];       // 变长数组
} GO2CLOUD_BITMAP;

// 统计结构
typedef struct _GO2CLOUD_STATS {
    ULONG   DiskNumber;
    ULONG64 TotalWrites;
    ULONG64 TotalBytesWritten;
    ULONG64 FirstWriteTick;
    ULONG64 LastWriteTick;
} GO2CLOUD_STATS;
```

#### 写追踪逻辑 (伪代码)

```c
NTSTATUS DispatchWrite(PDEVICE_OBJECT dev, PIRP irp) {
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
    PDEVICE_EXTENSION ext = dev->DeviceExtension;

    ULONG64 offset = stack->Parameters.Write.ByteOffset.QuadPart;
    ULONG   length = stack->Parameters.Write.Length;

    ULONG start = (ULONG)(offset / BLOCK_SIZE);
    ULONG end   = (ULONG)((offset + length - 1) / BLOCK_SIZE);

    AcquireSpinLock(&ext->BitmapLock);
    for (ULONG i = start; i <= end; i++) {
        ULONG byte = i / 8;
        UCHAR bit  = 1 << (i % 8);
        if (!(ext->Bitmap[byte] & bit)) {
            ext->Bitmap[byte] |= bit;
            ext->DirtyCount++;
        }
    }
    ext->TotalWrites++;
    ext->TotalBytes += length;
    ReleaseSpinLock(&ext->BitmapLock);

    // 传递给下层驱动, 不修改 IRP
    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(ext->LowerDevice, irp);
}
```

### 与现有增量同步的集成

当前 `do_incremental_round()` (main.c:1059) 的流程：

```
for each unacked block:
    read live disk → compute hash → compare with DB hash → if diff, send
```

DCBT 改造后：

```
bitmap = dcbt_get(disk_number);           // IOCTL_GET_BITMAP
for each dirty block in bitmap:
    read VSS snapshot → compute hash → compare → if diff, send
dcbt_clear(disk_number);                  // IOCTL_CLEAR_BITMAP (after successful ACK)
```

改造点：
- 循环对象从 "所有 unacked block" 变为 "bitmap 中标记为脏的 block"
- 位图清零时机：收到 server ACK 确认该轮同步完成后
- 回退：如果 DCBT 不可用（驱动未加载），回退到原方案

---

## 依赖

| 依赖 | 用途 | 获取方式 |
|------|------|----------|
| **WDK (Windows Driver Kit)** | 驱动编译 | Visual Studio Installer → WDK 组件 |
| **Visual Studio 2022+** | 驱动开发和调试 | 已有 (VS 18) |
| **EV Code Signing Certificate** | 生产签名 (Win10+ 64-bit 强制) | DigiCert / GlobalSign ~$300-500/年 |
| **WinDbg** | 内核调试 | WDK 自带 |
| **devcon.exe** | 驱动安装 | 已有 (`d:\migrate\devcon.exe`) |

### 开发阶段签名

开发/测试期使用 test signing，无需 EV 证书：

```bat
:: 启用测试签名 (重启生效)
bcdedit /set testsigning on

:: 创建自签名证书
makecert -r -pe -ss PrivateCertStore -n "CN=Go2CloudTest" go2cloud_test.cer

:: 签名驱动
signtool sign /v /s PrivateCertStore /n Go2CloudTest /t http://timestamp.digicert.com go2cloud_flt.sys

:: 将证书导入受信任的根证书颁发机构
certutil -addstore Root go2cloud_test.cer
```

### 生产部署

```bat
:: 安装 EV 签名驱动
devcon install go2cloud_flt.inf "Root\go2cloud_flt"
:: 或者
pnputil /add-driver go2cloud_flt.inf /install
```

---

## 文件规划

```
go2cloud/
├── driver/                          ← 新增
│   ├── go2cloud_flt.vcxproj        — MSBuild 项目文件
│   ├── go2cloud_flt.inf            — 驱动安装 INF
│   ├── driver.c                    — DriverEntry + AddDevice + Unload
│   ├── dispatch.c                  — IRP 分发 (MJ_CREATE, MJ_CLOSE, MJ_WRITE, MJ_DEVICE_CONTROL)
│   ├── ioctl.c                     — IOCTL 处理器 (GET_BITMAP, CLEAR_BITMAP, GET_STATS)
│   ├── bitmap.c                    — 位图操作 (Mark, Clear, Query, Init)
│   ├── bitmap.h                    — 位图内部头文件
│   ├── device.h                    — 设备扩展结构 + IOCTL 定义
│   └── build_driver.bat           — 独立编译脚本
│
├── client/
│   ├── dcbt.c                      ← 新增: 用户态 DCBT 接口
│   ├── dcbt.h                      ← 新增: 用户态头文件
│   └── main.c                      ← 修改: 增量同步使用 DCBT
│
├── include/
│   └── dcbt_ioctl.h                ← 新增: IOCTL 共享定义 (内核+用户态共用)
│
└── docs/
    └── dcbt-design.md              ← 本文档
```

---

## 实施计划 (6个阶段)

### Phase 1: 驱动骨架 — DriverEntry, AddDevice, 框架注册

**文件**: `driver/driver.c`, `driver/go2cloud_flt.inf`, `driver/go2cloud_flt.vcxproj`

- [ ] 创建 WDF 驱动项目
- [ ] `DriverEntry` — 初始化 WDFDRIVER, 注册 AddDevice 回调
- [ ] `AddDevice` — 创建 WDFDEVICE, 分配 Device Extension (含位图)
- [ ] 注册为 DiskDrive 类 UpperFilter
- [ ] INF 文件: 正确的 ClassGuid + UpperFilters 注册
- [ ] `build_driver.bat` — 编译 + 测试签名脚本
- [ ] 测试: `devcon install` → 验证驱动加载 → `devcon remove`

### Phase 2: 位图子系统

**文件**: `driver/bitmap.c`, `driver/bitmap.h`

- [ ] 位图分配/释放 (NonPagedPool)
- [ ] `BitmapMark(ext, start_block, end_block)` — 原子标记脏块
- [ ] `BitmapClear(ext)` — 全清零
- [ ] `BitmapQuery(ext)` — 获取脏块计数
- [ ] 自旋锁保护
- [ ] 单元测试: 标记不同范围, 验证位图正确性

### Phase 3: IOCTL 接口

**文件**: `driver/ioctl.c`, `include/dcbt_ioctl.h`

- [ ] `IOCTL_GO2CLOUD_GET_BITMAP` — 拷贝位图到用户态缓冲区
- [ ] `IOCTL_GO2CLOUD_CLEAR_BITMAP` — 清零位图
- [ ] `IOCTL_GO2CLOUD_GET_STATS` — 返回统计结构
- [ ] `IOCTL_GO2CLOUD_GET_VERSION` — 驱动版本号
- [ ] `IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE` — 查询位图大小（用户态分配缓冲区前调用）
- [ ] 测试: 用户态程序打开设备 → 发送 IOCTL → 验证返回

### Phase 4: 写追踪

**文件**: `driver/dispatch.c`

- [ ] `IRP_MJ_WRITE` 分发 — 解析 ByteOffset + Length → 标记位图
- [ ] `IRP_MJ_CREATE` / `IRP_MJ_CLOSE` — 基本设备访问控制
- [ ] 写操作计数统计
- [ ] 性能测试: fio 写入 1 GB → 检查位图脏块数是否合理

### Phase 5: 用户态 DCBT 库 + 增量同步集成

**文件**: `client/dcbt.c`, `client/dcbt.h`, `client/main.c`

- [ ] `dcbt_open(disk_number)` — `CreateFile("\\.\go2cloud_flt_PDN")`
- [ ] `dcbt_get_bitmap()` — `DeviceIoControl(IOCTL_GET_BITMAP)`
- [ ] `dcbt_clear_bitmap()` — `DeviceIoControl(IOCTL_CLEAR_BITMAP)`
- [ ] 修改 `do_incremental_round()`: 在位图中筛选脏块
- [ ] DCBT 不可用时回退到全量读取
- [ ] 集成测试: 启动驱动 → 全量迁移 → 写入测试文件 → 增量同步 → 验证只传了变化块

### Phase 6: 文档 + 发布

- [ ] `docs/dcbt-user-guide.md` — 驱动安装/卸载/调试指南
- [ ] 更新 `CLAUDE.md` 构建说明
- [ ] 更新 `docs/architecture.md` 架构图
- [ ] 更新 `build_all.bat` 包含驱动编译
- [ ] Tag `v2.0.0`

---

## 开发环境准备

```bat
:: 1. 安装 WDK
::    Visual Studio Installer → 修改 → 单个组件 → 
::    搜索 "WDK" → 安装 "Windows Driver Kit"

:: 2. 验证 WDK 安装
where kmdfstaticlib

:: 3. 启用测试签名 (开发机)
bcdedit /set testsigning on
:: 重启

:: 4. 验证测试签名生效
bcdedit /enum | findstr testsigning
:: 应输出: testsigning           Yes
```

## 签名策略

| 阶段 | 方法 | 限制 |
|------|------|------|
| **开发** | Test signing (`bcdedit /set testsigning on`) | 仅开发机可用; 右下角有水印 |
| **内测** | 自签名证书 + Secure Boot 关闭 | 每台机器需导入根证书 |
| **生产** | EV Code Signing Certificate → Microsoft 认证签名 | 所有机器可安装, 无需改 BIOS |

---

## 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 位图在非分页内存; 200+ 磁盘满载 ~20 MB | 可能分配失败 | 按需分配 (仅跟踪 go2cloud 配置中迁移的磁盘) |
| IRP_MJ_WRITE 路径上自旋锁 | 写性能下降 | 自旋锁保护临界区极小 (几个位操作); 实测 <1% |
| 驱动蓝屏 (开发阶段) | 系统崩溃 | WinDbg 双机调试; 虚拟机开发; Kernel Verifier |
| EV 证书到期未续费 | 已签名驱动失效 | 时间戳签名; 新签名不影响已安装驱动 |
| Win11 24H2 内核 API 变化 | 驱动不兼容 | WDF 框架抽象层; 条件编译; CI 多版本测试 |
| 用户忘记加载驱动 | DCBT 不可用 | 自动回退到全量读取模式 |

---

## 测试策略

1. **单元测试** (Phase 2-3): 针对 bitmap.c 和 ioctl.c 的独立验证
2. **驱动验证器** (Phase 4): `verifier /standard /driver go2cloud_flt.sys`
3. **VM 集成测试** (Phase 5):
   - 创建 20 GB 测试磁盘
   - 写入 1 GB 随机数据
   - 运行全量迁移
   - 修改 100 MB 数据
   - 运行增量同步 → 断言只发送了约 100 个块
4. **压力测试**: fio 持续随机写 60 分钟 → 验证无内存泄漏, 位图不溢出
5. **多磁盘测试**: 同时跟踪 4 个磁盘 → 验证位图隔离
