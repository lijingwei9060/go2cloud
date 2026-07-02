# go2cloud — Block-Level Online Migration Tool

在线块级磁盘迁移工具。逐块读取源端磁盘，哈希去重、Zstd 压缩后经 TCP 发送到目标端写入。

- **client** — 源端（Windows），负责磁盘枚举、VSS 快照、块读取、哈希、压缩、发送
- **receiver** — 目标端（Linux），负责 TCP 监听、协议解码、块写入、ACK 响应

---

## 项目结构

```
go2cloud/
├── include/protocol.h         — 共享协议常量
├── client/                    — 源端 (Windows + Linux)
│   ├── main.c                 — 入口、配置解析、迁移循环、子命令
│   ├── volume.c/.h            — 磁盘枚举 (PhysicalDriveN)
│   ├── block_io.c/.h          — 块级磁盘读取
│   ├── vss.c/.h               — VSS 快照管理 (仅 Windows, COM)
│   ├── hash.c/.h              — 64-bit 块哈希
│   ├── msgpack.c/.h           — MsgPack 编码器
│   ├── wire.c/.h              — 4 层有线协议
│   ├── queue.c/.h             — 发送队列 (环形缓冲)
│   ├── pool.c/.h              — 连接池 (7 并发)
│   ├── timer.c/.h             — 定时器管理
│   ├── sqlite.c/.h            — SQLite 块跟踪 (WAL 模式)
│   └── log.c/.h               — 日志
├── server/                    — 目标端 (Linux + Windows)
│   ├── main.c                 — TCP 服务器, select() 事件循环
│   ├── session.c/.h           — 会话管理 (最多 16 个)
│   ├── protocol_decoder.c/.h  — 协议解码器
│   ├── block_writer.c/.h      — 目标磁盘写入
│   ├── ack.c/.h               — ACK 响应生成
│   └── log.c/.h               — 日志
├── test/test_protocol.c       — 协议单元测试
├── tools/hash_block.c         — 块哈希命令行工具
└── docs/                      — 设计文档
```

---

## 依赖

### client (Windows)

| 依赖 | 用途 |
|------|------|
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | MSVC 编译器 (Community 版即可) |
| [vcpkg](https://github.com/microsoft/vcpkg) | 包管理 |
| zstd | Zstandard 压缩 |
| sqlite3 | 块跟踪数据库 |

Windows SDK 自带: `ws2_32.lib`, `ole32.lib`, `vssapi.lib`

### receiver (Linux)

| 依赖 | 安装命令 (Debian/Ubuntu) |
|------|--------------------------|
| gcc / make | `apt install build-essential` |
| libzstd-dev | `apt install libzstd-dev` |

---

## 安装依赖

### Windows (client)

```powershell
# 1. 安装 Visual Studio 2026 Community
#    勾选 "Desktop development with C++" 工作负载

# 2. 安装 vcpkg
git clone https://github.com/microsoft/vcpkg.git d:\vcpkg
cd d:\vcpkg
.\bootstrap-vcpkg.bat

# 3. 安装依赖包
.\vcpkg install zstd sqlite3 --triplet x64-windows
```

### Linux (receiver)

```bash
# Debian / Ubuntu
sudo apt update
sudo apt install build-essential libzstd-dev

# RHEL / CentOS / Fedora
sudo dnf install gcc make libzstd-devel
```

---

## 编译

### client.exe (Windows MSVC)

```bat
:: 初始化 MSVC 环境
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsx86_amd64.bat"

:: 编译 .c 文件 (vss.c 需用 C++ 编译, 因为 VSS COM 头文件依赖 C++)
cl /c /O2 /utf-8 client\main.c client\log.c client\hash.c client\msgpack.c client\wire.c client\queue.c client\pool.c client\timer.c client\sqlite.c client\volume.c client\block_io.c /Iinclude /Id:\vcpkg\installed\x64-windows\include
cl /c /O2 /utf-8 /Tpclient\vss.c /Iinclude /Id:\vcpkg\installed\x64-windows\include

:: 链接
link /OUT:client.exe *.obj /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib
```

或使用项目自带脚本 (需要 vcpkg 在 `d:\vcpkg`):

```bat
build_quick.bat      :: 仅编译 client.exe
build_all.bat        :: 编译 client.exe + receiver.exe
```

### receiver (Linux GCC)

```bash
# 方法 1: 使用 Makefile
make server

# 方法 2: 直接编译
gcc -O2 -o receiver server/*.c -Iinclude -lzstd -lpthread
```

### 项目 Makefile

```bash
make server          # Linux: 编译 receiver
make client          # Linux: 编译 client (含 SQLite, 不含 VSS)
make all             # 编译 server + client
make clean           # 清理
```

Linux 端 client 编译需要额外安装 `libsqlite3-dev`:
```bash
sudo apt install libsqlite3-dev
```

---

## 基础使用

### 1. 目标端启动 receiver

> 注意不要往真实硬盘写入，防止误操作

创建sparc目标文件
```shell
truncate -s 200G /tmp/disk0.img
truncate -s 200G /tmp/disk1.img
```

创建 `receiver.json`:
```json
{
    "Listen": {
        "Address": "0.0.0.0",
        "Port": 8889
    },
    "Target": {
        "Disks": {
            "0": "/tmp/disk0.img",
            "1": "/tmp/disk1.img"
        }
    },
    "Log": {
        "Level": 3
    }
}
```

```bash
./receiver --config receiver.json
```

### 2. 源端全量同步

创建 `user.json`:
```json
{
    "RateLimitMB": 100,
    "ZstdLevel": 7
}
```

```bat
client.exe 10.0.0.1:8889 user.json
```

### 3. 源端增量同步

全量同步完成后定期执行，仅发送变化的块:
```bat
client.exe incsync 10.0.0.1:8889 user.json
```

### 4. 查看块跟踪信息

```bat
client.exe blockinfo client/tracker.db
client.exe blockinfo --history client/tracker.db
client.exe blockinfo 0 1048576 client/tracker.db
```

### 5. 本地模拟 (无网络)

```bat
client.exe dryrun user.json
```

### 6. 其它子命令

```bat
client.exe info                  :: 显示磁盘分区信息
client.exe test_vss              :: 测试 VSS 快照
client.exe vss_query             :: 查询现有快照
client.exe vss_delete --all      :: 删除所有快照
client.exe sentbytes             :: 查询已确认字节数
client.exe --help                :: 显示帮助
```

---

## 配置参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `RateLimitMB` | int | 100 | 速率限制 (MB/s), 0 不限制 |
| `ZstdLevel` | int | 7 | Zstd 压缩级别 (1-22) |
| `TailSend` | bool | false | 全量后持续增量同步 |
| `SkipDisks` | int[] | [] | 跳过的磁盘编号 |
| `LogLevel` | int | 3 | 日志级别 (1-5) |
| `LogPath` | string | "" | 日志文件路径 (空=仅控制台) |

---

## 协议

4 层有线协议:
1. **TCP 分帧** — 4 字节大端长度前缀
2. **魔数** — 3 字节 `"abc"` (0x61 0x62 0x63)
3. **压缩** — Zstd
4. **编码** — MsgPack fixmap(3): `{0: devno, 1: offset, 2: data}`

控制消息: `"ctlIncremental"` / `"ctlEndIncremental"` (纯文本, 不压缩)。

服务端返回 20 字节 `{type, devno, size, offset}` ACK 包。

详见 `docs/protocol.md`。
