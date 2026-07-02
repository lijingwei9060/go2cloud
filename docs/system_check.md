# System Environment Check — User Manual

## Overview

`checkenv` runs a comprehensive pre-migration audit of the source machine. It validates
all prerequisites for a successful block-level migration to Tencent Cloud and reports
pass/warn/fail for each check. Run it **before** starting a migration to catch issues early.

## Usage

```
client.exe checkenv [driver_dir]
```

- `driver_dir` (optional): path to the VirtIO driver directory. Default: `drivers`.
  If omitted, the VirtIO driver check is skipped.

## Checks Performed

| # | Check | What It Validates | Failure Impact |
|---|-------|-------------------|----------------|
| 1 | **Admin rights** | Process runs as Administrator (or root on Linux) | Cannot open PhysicalDrive — migration impossible |
| 2 | **OS version** | Windows 7+ / Server 2008 R2+ | Older versions untested, may have API gaps |
| 3 | **VSS service** | Volume Shadow Copy service is running | VSS snapshots fail — cannot get consistent disk reads |
| 4 | **Disk access** | All PhysicalDriveN disks can be opened for read | Cannot read source disk blocks |
| 5 | **Disk space** | System drive has ≥ 1 GB free | SQLite tracker.db may run out of space |
| 6 | **BIOS mode** | UEFI or Legacy BIOS detected | Must match target VM boot mode |
| 7 | **VirtIO drivers** | viostor, netkvm, qxldod are in driver store | Target VM will BSOD (0x7B) without viostor |

## Example Output

### All Pass

```
  System Environment Check
  ========================

  [PASS] Admin rights       running with administrator privileges
  [PASS] OS version         Windows 10.0 (build 22631) — supported
  [PASS] VSS service        VSS service is running
  [PASS] Disk access        3 disk(s) accessible: C: (238.47 GB) D: (931.51 GB) E: (465.76 GB)
  [PASS] Disk space         C: 156 GB free / 238 GB total — adequate
  [PASS] BIOS mode          UEFI firmware detected — target VM must use UEFI boot
  [PASS] VirtIO drivers     all 3 VirtIO drivers (viostor, netkvm, qxldod) are pre-installed

  Summary: 7 pass, 0 warn, 0 fail

  ALL PASS — system is ready for migration.
```

### With Failures

```
  System Environment Check
  ========================

  [PASS] Admin rights       running with administrator privileges
  [PASS] OS version         Windows 10.0 (build 22631) — supported
  [PASS] VSS service        VSS service is running
  [PASS] Disk access        2 disk(s) accessible: C: (238.47 GB) D: (931.51 GB)
  [PASS] Disk space         C: 156 GB free / 238 GB total — adequate
  [PASS] BIOS mode          UEFI firmware detected — target VM must use UEFI boot
  [FAIL] VirtIO drivers     NO VirtIO drivers installed — target VM will BSOD (0x7B). Run: inject_driver

  Summary: 6 pass, 0 warn, 1 fail

  FAIL — 1 check(s) failed. Fix issues above before migration.
```

## Individual Checks

For targeted checks, use the single-purpose subcommands:

| Subcommand | Purpose |
|------------|---------|
| `isbios` | Detect firmware type only |
| `test_vss` | Test VSS snapshot creation |
| `check <disk>` | Test disk accessibility for one disk |
| `check_drivers` | Check VirtIO driver status only |
| `vss_query` | List existing VSS snapshots |
| `info` | Enumerate all disks |

## Interpreting Results

### PASS (green)
The check passed. No action needed.

### WARN (yellow)
The check has a non-critical issue. Migration **may** still work, but review the warning:
- `VSS service starting...` — wait for the service to reach RUNNING state, then re-run.
- `2/3 access OK` — one disk is inaccessible; check disk health or SkipDisks config.
- `Low disk space` — free up space on the system drive to avoid SQLite errors.

### FAIL (red)
Migration will likely fail. Fix these before proceeding:
- **Not admin** → restart the command prompt as Administrator.
- **VSS stopped** → run `net start VSS` from an admin prompt.
- **No disks** → check disk controller drivers, verify PhysicalDriveN exists.
- **No VirtIO drivers** → run `client.exe inject_driver drivers`.
- **Old Windows version** → upgrade to Windows 7+ or use a different migration method.

## Prerequisites

- **Administrator privileges** — most checks require elevation.
- **VirtIO driver files** (optional) — needed for the driver check. Copy from go2tencentcloud project.

## Automation

The checkenv command is designed for use in scripts. The exit code indicates overall status:

| Exit Code | Meaning |
|-----------|---------|
| 0 | All checks passed — ready for migration |
| -1 | One or more checks failed — migration not recommended |

## Log Output

In addition to the console summary, all check results are written to the log file
(configured via `Log.Path` in `user.json` or default stderr). The log includes
detailed diagnostic messages for each check at `LOG_INFO` level.

## References

- [VSS Service Troubleshooting](https://learn.microsoft.com/en-us/windows-server/storage/file-server/volume-shadow-copy-service)
- [Windows PhysicalDrive Access](https://learn.microsoft.com/en-us/windows/win32/fileio/opening-a-file-for-reading-or-writing)
- [Tencent Cloud Pre-migration Checks](https://www.tencentcloud.com/document/product/213/35649)
