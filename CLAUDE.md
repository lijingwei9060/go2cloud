# go2cloud — Block-Level Online Migration Tool

> **每次编译后，必须将 client.exe 和 receiver.exe 拷贝到 `d:\migrate` 目录。**

## Build (Windows MSVC)

```bat
:: One-time: set up vcpkg dependencies
:: vcpkg install zstd sqlite3 --triplet x64-windows

:: Build client
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsx86_amd64.bat"
cl /c /O2 /utf-8 client\main.c client\log.c client\hash.c client\msgpack.c client\wire.c client\queue.c client\pool.c client\timer.c client\sqlite.c client\volume.c client\block_io.c /Iinclude /Id:\vcpkg\installed\x64-windows\include
cl /c /O2 /utf-8 /Tpclient\vss.c /Iinclude /Id:\vcpkg\installed\x64-windows\include
link /OUT:client.exe *.obj /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib sqlite3.lib ole32.lib vssapi.lib ws2_32.lib

:: Build server
cl /O2 /utf-8 /Fe:receiver.exe server\*.c /Iinclude /Id:\vcpkg\installed\x64-windows\include /link /LIBPATH:d:\vcpkg\installed\x64-windows\lib zstd.lib ws2_32.lib

:: Deploy to d:\migrate (必须执行)
copy /Y client.exe d:\migrate\
copy /Y receiver.exe d:\migrate\
```

Key: `vss.c` must be compiled as C++ (`/Tp` flag) because the Windows VSS COM headers (`vswriter.h`, `vsbackup.h`) use C++-only constructs (`__declspec(uuid(...))`, `interface ... : IUnknown`).

vcpkg root: `d:\vcpkg`; triplet: `x64-windows`.

## Project Structure

```
go2cloud/
├── include/protocol.h        — Shared protocol constants (client + server)
├── client/                   — Source-side block transfer (Windows + Linux)
│   ├── main.c                — Entry, config parsing, migration loop, subcommands
│   ├── volume.c/.h           — Disk enumeration (PhysicalDriveN on Win, /dev/sdX on Linux)
│   ├── block_io.c/.h         — Block-level disk read (Win32 CreateFile, Linux open/pread64)
│   ├── vss.c/.h              — VSS snapshot management (Windows-only, COM)
│   ├── hash.c/.h             — Custom 64-bit xxHash-like hash
│   ├── msgpack.c/.h          — MsgPack encoder (fixmap(3) block messages)
│   ├── wire.c/.h             — 4-layer wire protocol: TCP 4B BE len → "abc" magic → Zstd → MsgPack
│   ├── queue.c/.h            — Thread-safe ring buffer send queue (256 entries)
│   ├── pool.c/.h             — Socket connection pool (7 concurrent TCP conns)
│   ├── timer.c/.h            — Timer manager (retransmit/reconnect/incremental/action)
│   ├── sqlite.c/.h           — SQLite block tracking (T_BLOCK table, WAL mode)
│   └── log.c/.h              — Thread-safe logging (cross-platform)
├── server/                   — Target-side receiver (Linux + Windows)
│   ├── main.c                — TCP server, select() event loop, config parsing
│   ├── session.c/.h          — Client session management (max 16, 4MB recv_buf)
│   ├── protocol_decoder.c/.h — 4-layer protocol decoder (TCP frame → "abc" → Zstd → MsgPack)
│   ├── block_writer.c/.h     — Target disk write (lseek+write, periodic fsync, BINLOG mon)
│   ├── ack.c/.h              — 20-byte ACK response generator
│   └── log.c/.h              — Thread-safe logging
├── test/test_protocol.c      — Protocol unit tests
├── docs/                     — Design documents
│   ├── architecture.md       — Architecture overview
│   ├── client.md             — Client design doc
│   ├── protocol.md           — Wire protocol spec
│   └── receiver.md           — Receiver usage manual
└── Makefile                  — Cross-platform build (GCC / MSVC targets)
```

## Key Architecture Decisions

- **Receiver is stateless**: No metadata, no SQLite. Just decode, write, ACK. All state management is client-side.
- **Dual-layer incremental sync**: Layer 1 — 18s concurrent timer during full sync (read live disk, hash compare). Layer 2 — post-full-sync convergence loop (reset ack=0 for all blocks, re-read all from live disk).
- **Append-only T_BLOCK**: INSERT OR REPLACE on (devno, offset). ACK lifecycle: upsert(ack=0) → server ACK → mark ack=1 → reset ack=0 during incremental → mark ack=1 again.
- **Crash consistency**: Final state = source pulled power suddenly. NTFS journal replay on target boot fixes filesystem inconsistencies.
- **Fisher-Yates shuffle**: Randomizes unacked block processing order each incremental round (avoids disk cache bias).
- **121s cooldown**: Minimum interval before re-reading the same block's hash from live disk (`RETRANSMIT_MIN_INTERVAL_SEC`).
- **Convergence control**: 3 exit conditions — perfect (unacked==0), stable (3 zero-change rounds), timeout (60 rounds max).

## Subcommands

```
client.exe info                    — Enumerate disks
client.exe hash <file>             — Compute block hash
client.exe check <disk>            — Check disk accessibility
client.exe isbios                  — Detect UEFI/Legacy firmware
client.exe test_vss                — Create VSS persistent snapshot
client.exe vss_query               — List existing VSS snapshots
client.exe vss_delete <guid>       — Delete specific snapshot
client.exe vss_delete --all        — Delete all snapshots
client.exe dryrun [config.json]    — Dry-run: simulate full migration locally (no server)
client.exe <ip:port> [config.json] — Real migration
```

## Configuration (user.json)

```json
{
    "Log": {"Level": 2, "Path": "client.log"},
    "TailSend": 0,
    "HasDump": 1,
    "DbPath": "tracker.db",
    "SkipDisks": [3, 4],
    "Disks": {"0": "\\\\.\\PhysicalDrive0", "1": "\\\\.\\PhysicalDrive1"}
}
```

- `TailSend: 1` → incremental mode (requires ctlIncremental handshake, 18s convergence loop)
- `TailSend: 0` → full sync only (single pass then ctlEndIncremental)
- Must run as Administrator for PhysicalDrive access.
