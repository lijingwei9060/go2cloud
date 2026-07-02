# VirtIO Driver Injection — User Manual

## Overview

When migrating a Windows physical machine to Tencent Cloud KVM, the source hardware drivers
(Intel/AMD SATA, onboard NIC, etc.) are incompatible with KVM's virtio devices. Without VirtIO
drivers pre-installed, the target VM will fail with `0x7B INACCESSIBLE_BOOT_DEVICE` BSOD.

The `inject_driver` command pre-stages VirtIO drivers into the Windows driver store **before**
migration. This is "dormant injection": the drivers sit idle on the source machine (where virtio
hardware doesn't exist), but when the VM boots on KVM and Windows enumerates the virtio devices,
it finds matching drivers already in the store and loads them automatically.

## Three Drivers

| Driver   | Hardware ID                                   | Purpose                     | Critical? |
|----------|-----------------------------------------------|-----------------------------|-----------|
| viostor  | `PCI\VEN_1AF4&DEV_1001&SUBSYS_00021AF4&REV_00` | VirtIO SCSI storage         | Yes — missing = 0x7B BSOD |
| netkvm   | `PCI\VEN_1AF4&DEV_1000&SUBSYS_00011AF4&REV_00` | VirtIO network adapter      | Yes — missing = no network |
| qxldod   | `PCI\VEN_1B36&DEV_0100&SUBSYS_11001AF4`        | QXL display adapter         | No — affects VNC console only |

### Start Types

- **viostor**: `SERVICE_BOOT_START` (0) — loaded by the Windows boot loader early in boot.
  This is what prevents the 0x7B BSOD.
- **netkvm**: `SERVICE_DEMAND_START` (3) — loaded on demand.
- **qxldod**: `SERVICE_DEMAND_START` (3) — loaded on demand.

## Directory Structure

The tool expects drivers in the following layout (matching the go2tencentcloud project):

```
drivers/
├── viostor/
│   └── Install/
│       ├── Win7/amd64/viostor.inf
│       ├── Win8/amd64/viostor.inf
│       ├── Win8.1/amd64/viostor.inf
│       ├── Win10/amd64/viostor.inf
│       ├── 2k12/amd64/viostor.inf
│       ├── 2k12R2/amd64/viostor.inf
│       └── 2k16/amd64/viostor.inf
├── netkvm/
│   └── Install/
│       └── (same subfolder layout)
└── qxldod/
    └── (same subfolder layout)
```

The tool automatically selects the best folder for your Windows version.

## Commands

### Check Driver Status

```
client.exe check_drivers [driver_dir]
```

Scans the driver store and reports which of the three VirtIO drivers are already installed.
Does **not** install anything itself (though as a side-effect, `SetupCopyOEMInfW` may install
a driver that was found on disk but not yet in the store — this is harmless and desired).

Example output:
```
Checking VirtIO drivers in: drivers
  viostor     [INSTALLED]
  netkvm      [INSTALLED]
  qxldod      [MISSING] driver not in store

Status: 2/3 drivers installed
Remedy: run  client.exe inject_driver drivers
```

### Install Drivers

```
client.exe inject_driver [driver_dir]
```

Pre-installs all three VirtIO drivers into the driver store. Idempotent — safe to run multiple
times (already-installed drivers are skipped).

Example output:
```
Injecting VirtIO drivers from: drivers
  viostor     [ALREADY INSTALLED]
  netkvm      [INSTALLED NOW]
  qxldod      [INSTALLED NOW]

All VirtIO drivers are now in the driver store.
When the VM boots on Tencent Cloud KVM, Windows will load them automatically.
```

## How It Works (Technical Details)

The tool uses the Windows Setup API (`setupapi.dll`):

1. **INF Discovery**: Scans `<driver_dir>/<name>/Install/` for version-specific subdirectories
   matching the current Windows version (via `RtlGetVersion`).

2. **Driver Store Copy**: Calls `SetupCopyOEMInfW(inf_path, NULL, SPOST_PATH, SP_COPY_NOOVERWRITE, ...)`.
   This copies the INF, CAT, and SYS files into `C:\Windows\INF\` and registers them in the
   driver store database (`C:\Windows\System32\drivers\`).

3. **Idempotency**: If the exact same INF package is already in the store, the API returns
   `FALSE` with `ERROR_FILE_EXISTS` — treated as success (already installed).

### Why This Works

Windows driver binding is based on PnP hardware ID matching:
- On source hardware: PCI bus enumerates Intel/AMD devices; virtio INF doesn't match → driver not loaded.
- On KVM target: PCI bus enumerates virtio devices (`VEN_1AF4`); Windows searches the driver store,
  finds the pre-staged viostor/netkvm/qxldod INF, and loads them.

No reboot is needed on the source machine — the drivers are simply dormant until the hardware appears.

## Prerequisites

- **Administrator privileges** — required for `SetupCopyOEMInfW` (driver store write access).
- **Driver files** — the INF/CAT/SYS files must be present on disk (copied from the
  go2tencentcloud project or downloaded from the Fedora VirtIO driver ISO).

## Error Recovery

If `inject_driver` fails:
1. Verify the driver directory structure matches the expected layout.
2. Check that INF files exist for your Windows version.
3. Run `client.exe check_drivers` to see which specific driver failed.
4. Manually install via `pnputil /add-driver <inf_path>` as a fallback.

## References

- [Windows Setup API — SetupCopyOEMInf](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupcopyoeminfw)
- [KVM VirtIO Drivers](https://docs.fedoraproject.org/en-US/quick-docs/creating-windows-virtual-machines-using-virtio-drivers/)
- [Tencent Cloud Windows Migration — CVM Service Migration](https://www.tencentcloud.com/document/product/213/35649)
