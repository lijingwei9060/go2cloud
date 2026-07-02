/*
 * driver.c — go2cloud_flt KMDF upper-filter driver
 *
 * Registered as UpperFilter on the DiskDrive class.
 * For each physical disk, allocates a 1 MB granularity dirty-block bitmap
 * and intercepts write IRPs to track changed blocks.
 *
 * A single control device (\\.\go2cloud_flt) exposes per-disk bitmaps
 * via IOCTLs so user-mode client.exe can query dirty blocks.
 */

#include "device.h"
#include "bitmap.h"
#include <ntddstor.h>
#include <ntdddisk.h>

#pragma warning(disable: 4054)
#pragma warning(disable: 4055)

CONTROL_DEVICE_CONTEXT *g_CtrlCtx = NULL;
static ULONG g_BootDiskNumber = (ULONG)-1;

/* Forward declarations for PnP callbacks */
static NTSTATUS EvtDevicePrepareHardware(WDFDEVICE, WDFCMRESLIST, WDFCMRESLIST);
static NTSTATUS EvtDeviceReleaseHardware(WDFDEVICE, WDFCMRESLIST);
static VOID EvtDiskInitWorkItem(WDFWORKITEM);

/* ---------------------------------------------------------------
 * GetBootDiskNumber — read the boot disk rdisk(N) from registry
 * --------------------------------------------------------------- */
static ULONG GetBootDiskNumber(VOID)
{
	NTSTATUS status;
	HANDLE hKey = NULL;
	UNICODE_STRING keyName, valName;
	OBJECT_ATTRIBUTES oa;
	UCHAR buf[256];
	PKEY_VALUE_PARTIAL_INFORMATION info;
	ULONG resultLen;
	ULONG diskNum = (ULONG)-1;

	RtlInitUnicodeString(&keyName,
		L"\\Registry\\Machine\\System\\CurrentControlSet\\Control");
	InitializeObjectAttributes(&oa, &keyName,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	status = ZwOpenKey(&hKey, KEY_READ, &oa);
	if (!NT_SUCCESS(status))
		return diskNum;

	RtlInitUnicodeString(&valName, L"SystemBootDevice");
	info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
	status = ZwQueryValueKey(hKey, &valName, KeyValuePartialInformation,
		buf, sizeof(buf), &resultLen);
	ZwClose(hKey);
	if (!NT_SUCCESS(status))
		return diskNum;
	if (info->Type != REG_SZ && info->Type != REG_EXPAND_SZ)
		return diskNum;

	/* Parse rdisk(N) from ARC path like multi(0)disk(0)rdisk(0)partition(1) */
	PWCHAR data = (PWCHAR)info->Data;
	PWCHAR end = data + info->DataLength / sizeof(WCHAR);
	for (PWCHAR p = data; p + 6 <= end; p++) {
		WCHAR c0 = p[0], c1 = p[1], c2 = p[2], c3 = p[3], c4 = p[4];
		if ((c0 == L'r' || c0 == L'R') &&
		    (c1 == L'd' || c1 == L'D') &&
		    (c2 == L'i' || c2 == L'I') &&
		    (c3 == L's' || c3 == L'S') &&
		    (c4 == L'k' || c4 == L'K') &&
		    p[5] == L'(') {
			p += 6;
			diskNum = 0;
			while (p < end && *p >= L'0' && *p <= L'9') {
				diskNum = diskNum * 10 + (*p - L'0');
				p++;
			}
			break;
		}
	}
	return diskNum;
}

/* ---------------------------------------------------------------
 * Global disk table helpers
 * --------------------------------------------------------------- */

VOID FilterRegisterDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx, ULONG disk_number,
                        FILTER_DEVICE_CONTEXT *ctx)
{
	if (!ctrl_ctx || disk_number >= DCBT_MAX_DISKS)
		return;

	WdfSpinLockAcquire(ctrl_ctx->TableLock);
	ctrl_ctx->DiskTable[disk_number] = ctx;
	WdfSpinLockRelease(ctrl_ctx->TableLock);
}

VOID FilterUnregisterDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx, ULONG disk_number)
{
	if (!ctrl_ctx || disk_number >= DCBT_MAX_DISKS)
		return;

	WdfSpinLockAcquire(ctrl_ctx->TableLock);
	ctrl_ctx->DiskTable[disk_number] = NULL;
	WdfSpinLockRelease(ctrl_ctx->TableLock);
}

/* FilterUnregisterDiskLocked — caller holds TableLock */
static VOID FilterUnregisterDiskLocked(CONTROL_DEVICE_CONTEXT *ctrl_ctx,
                                       ULONG disk_number)
{
	ctrl_ctx->DiskTable[disk_number] = NULL;
}

/*
 * FilterLookupDisk — returns ctx with BitmapLock HELD on success.
 * Caller MUST call FilterLookupRelease(ctx) to release BitmapLock.
 * Returns NULL (no lock held) if the disk is not registered.
 *
 * Holding BitmapLock from inside TableLock closes the UAF window:
 * EvtDeviceReleaseHardware acquires TableLock before BitmapLock,
 * so it cannot free ctx while a lookup caller holds BitmapLock.
 */
FILTER_DEVICE_CONTEXT *FilterLookupDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx,
                                        ULONG disk_number)
{
	if (!ctrl_ctx || disk_number >= DCBT_MAX_DISKS)
		return NULL;

	WdfSpinLockAcquire(ctrl_ctx->TableLock);
	FILTER_DEVICE_CONTEXT *ctx = ctrl_ctx->DiskTable[disk_number];
	if (ctx)
		WdfSpinLockAcquire(ctx->BitmapLock);
	WdfSpinLockRelease(ctrl_ctx->TableLock);

	return ctx;
}

VOID FilterLookupRelease(FILTER_DEVICE_CONTEXT *ctx)
{
	WdfSpinLockRelease(ctx->BitmapLock);
}

/* ---------------------------------------------------------------
 * Control device — created once in DriverEntry
 * --------------------------------------------------------------- */

static NTSTATUS CreateControlDevice(WDFDRIVER Driver)
{
	NTSTATUS status;
	WDFDEVICE ctrl_dev = NULL;
	WDF_IO_QUEUE_CONFIG queue_cfg;
	WDF_OBJECT_ATTRIBUTES obj_attrs;
	UNICODE_STRING dev_name = RTL_CONSTANT_STRING(DCBT_DEVICE_NAME);
	UNICODE_STRING sym_name = RTL_CONSTANT_STRING(DCBT_SYMLINK_NAME);

	PWDFDEVICE_INIT dev_init = WdfControlDeviceInitAllocate(
		Driver, WDF_NO_OBJECT_ATTRIBUTES);
	if (!dev_init)
		return STATUS_INSUFFICIENT_RESOURCES;

	WdfDeviceInitSetDeviceType(dev_init, FILE_DEVICE_UNKNOWN);
	WdfDeviceInitSetCharacteristics(dev_init, FILE_DEVICE_SECURE_OPEN, FALSE);
	WdfDeviceInitAssignName(dev_init, &dev_name);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&obj_attrs, CONTROL_DEVICE_CONTEXT);

	status = WdfDeviceCreate(&dev_init, &obj_attrs, &ctrl_dev);
	if (!NT_SUCCESS(status))
		return status;  /* dev_init consumed by WdfDeviceCreate */

	status = WdfDeviceCreateSymbolicLink(ctrl_dev, &sym_name);
	if (!NT_SUCCESS(status))
		goto cleanup;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_cfg, WdfIoQueueDispatchSequential);
	queue_cfg.EvtIoDeviceControl = EvtIoDeviceControl;

	status = WdfIoQueueCreate(ctrl_dev, &queue_cfg,
		WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status))
		goto cleanup;

	g_CtrlCtx = ControlGetContext(ctrl_dev);
	RtlZeroMemory(g_CtrlCtx->DiskTable, sizeof(g_CtrlCtx->DiskTable));

	status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &g_CtrlCtx->TableLock);
	if (!NT_SUCCESS(status))
		goto cleanup;

	WdfControlFinishInitializing(ctrl_dev);
	return STATUS_SUCCESS;

cleanup:
	WdfObjectDelete(ctrl_dev);
	return status;
}

/* ---------------------------------------------------------------
 * EvtDriverUnload
 * --------------------------------------------------------------- */

static VOID EvtUnload(WDFDRIVER Driver)
{
	UNREFERENCED_PARAMETER(Driver);

	if (g_CtrlCtx) {
		/* Lock order: TableLock → BitmapLock, per the established
		 * hierarchy.  Each disk has its own BitmapLock. */
		for (ULONG i = 0; i < DCBT_MAX_DISKS; i++) {
			FILTER_DEVICE_CONTEXT *ctx;

			WdfSpinLockAcquire(g_CtrlCtx->TableLock);
			ctx = g_CtrlCtx->DiskTable[i];
			if (ctx) {
				WdfSpinLockAcquire(ctx->BitmapLock);
				g_CtrlCtx->DiskTable[i] = NULL;
			}
			WdfSpinLockRelease(g_CtrlCtx->TableLock);

			if (ctx) {
				BitmapFree(ctx);
				WdfSpinLockRelease(ctx->BitmapLock);
			}
		}
		if (g_CtrlCtx->TableLock)
			WdfObjectDelete(g_CtrlCtx->TableLock);
	}

	DbgPrint("go2cloud_flt: unloaded\n");
}

/* ---------------------------------------------------------------
 * EvtDeviceAdd — PnP calls this for each DiskDrive in the system
 * --------------------------------------------------------------- */

static NTSTATUS
EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
	NTSTATUS status;
	WDFDEVICE device;
	WDF_OBJECT_ATTRIBUTES obj_attrs;
	WDF_IO_QUEUE_CONFIG queue_cfg;
	WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
	FILTER_DEVICE_CONTEXT *ctx;

	UNREFERENCED_PARAMETER(Driver);

	WdfFdoInitSetFilter(DeviceInit);

	/* Register PnP callbacks for hardware prepare/release */
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
	pnp_callbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
	pnp_callbacks.EvtDeviceReleaseHardware  = EvtDeviceReleaseHardware;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_callbacks);

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&obj_attrs, FILTER_DEVICE_CONTEXT);

	status = WdfDeviceCreate(&DeviceInit, &obj_attrs, &device);
	if (!NT_SUCCESS(status))
		return status;

	ctx = FilterGetContext(device);
	RtlZeroMemory(ctx, sizeof(*ctx));
	ctx->DiskNumber = (ULONG)-1;

	status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &ctx->BitmapLock);
	if (!NT_SUCCESS(status))
		return status;

	/* Parallel queue: write IRPs => EvtIoWrite; everything else => EvtIoDefault */
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_cfg, WdfIoQueueDispatchParallel);
	queue_cfg.EvtIoWrite   = EvtIoWrite;
	queue_cfg.EvtIoDefault = EvtIoDefault;

	status = WdfIoQueueCreate(device, &queue_cfg,
		WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	return status;
}

/* ---------------------------------------------------------------
 * EvtDevicePrepareHardware — defer disk init to work item so
 * IOCTL queries don't block or crash the PnP start path.
 * --------------------------------------------------------------- */

static NTSTATUS
EvtDevicePrepareHardware(WDFDEVICE Device,
                         WDFCMRESLIST ResourcesRaw,
                         WDFCMRESLIST ResourcesTranslated)
{
	UNREFERENCED_PARAMETER(ResourcesRaw);
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(Device);

	/* Already initialized on a previous D0 entry (e.g. sleep/resume).
	 * NonPagedPool bitmap survives D3, so skip the work item. */
	if (ctx->Bitmap)
		return STATUS_SUCCESS;

	WDF_WORKITEM_CONFIG workitem_cfg;
	WDF_OBJECT_ATTRIBUTES workitem_attrs;
	WDFWORKITEM workitem;
	NTSTATUS status;

	WDF_OBJECT_ATTRIBUTES_INIT(&workitem_attrs);
	workitem_attrs.ParentObject = Device;

	WDF_WORKITEM_CONFIG_INIT(&workitem_cfg, EvtDiskInitWorkItem);
	status = WdfWorkItemCreate(&workitem_cfg, &workitem_attrs, &workitem);
	if (!NT_SUCCESS(status))
		return status;

	WdfWorkItemEnqueue(workitem);
	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * EvtDiskInitWorkItem — runs asynchronously after device enters D0.
 * Queries disk number + size and allocates the dirty-block bitmap.
 * Skips the boot disk entirely to avoid 0x7B INACCESSIBLE_BOOT_DEVICE.
 * Wrapped in __try/__except to catch unexpected exceptions from
 * lower storage drivers.
 * --------------------------------------------------------------- */

static VOID
EvtDiskInitWorkItem(WDFWORKITEM WorkItem)
{
	WDFDEVICE Device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(Device);
	WDFIOTARGET target = WdfDeviceGetIoTarget(Device);
	STORAGE_DEVICE_NUMBER sd = {0};
	GET_LENGTH_INFORMATION len_info = {0};
	WDF_MEMORY_DESCRIPTOR out_desc;
	NTSTATUS status = STATUS_UNSUCCESSFUL;

	__try {
		WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc, &sd, sizeof(sd));
		status = WdfIoTargetSendIoctlSynchronously(
			target, NULL,
			IOCTL_STORAGE_GET_DEVICE_NUMBER,
			NULL, &out_desc, NULL, NULL);
		if (!NT_SUCCESS(status))
			__leave;

		ctx->DiskNumber = sd.DeviceNumber;

		/* Never query or track the boot disk — pass-through only */
		if (sd.DeviceNumber == g_BootDiskNumber) {
			DbgPrint("go2cloud_flt: Disk%u is boot disk, skipping\n",
				sd.DeviceNumber);
			__leave;
		}

		WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc, &len_info,
			sizeof(len_info));
		status = WdfIoTargetSendIoctlSynchronously(
			target, NULL,
			IOCTL_DISK_GET_LENGTH_INFO,
			NULL, &out_desc, NULL, NULL);
		if (!NT_SUCCESS(status))
			__leave;

		status = BitmapInit(ctx, sd.DeviceNumber,
			len_info.Length.QuadPart);
		if (!NT_SUCCESS(status))
			__leave;

		if (g_CtrlCtx)
			FilterRegisterDisk(g_CtrlCtx, ctx->DiskNumber, ctx);

		DbgPrint("go2cloud_flt: Disk%u ready — %u blocks, %u byte bitmap\n",
			ctx->DiskNumber, ctx->TotalBlocks, ctx->BitmapSize);
	}
	__except(EXCEPTION_EXECUTE_HANDLER) {
		DbgPrint("go2cloud_flt: exception during disk%u init, skipping\n",
			sd.DeviceNumber);
	}

	WdfObjectDelete(WorkItem);
}

/* ---------------------------------------------------------------
 * EvtDeviceReleaseHardware — disk going away; free bitmap
 * --------------------------------------------------------------- */

static NTSTATUS
EvtDeviceReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(Device);

	/* Lock ordering: TableLock → BitmapLock.
	 * Matches FilterLookupDisk which acquires BitmapLock under TableLock.
	 * This prevents FilterLookupDisk from returning ctx while we're
	 * tearing it down, closing the UAF window. */
	if (g_CtrlCtx)
		WdfSpinLockAcquire(g_CtrlCtx->TableLock);
	WdfSpinLockAcquire(ctx->BitmapLock);

	if (ctx->Bitmap && ctx->DiskNumber != (ULONG)-1) {
		if (g_CtrlCtx)
			FilterUnregisterDiskLocked(g_CtrlCtx, ctx->DiskNumber);
		BitmapFree(ctx);
	}

	WdfSpinLockRelease(ctx->BitmapLock);
	if (g_CtrlCtx)
		WdfSpinLockRelease(g_CtrlCtx->TableLock);

	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * DriverEntry
 * --------------------------------------------------------------- */

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	WDF_DRIVER_CONFIG config;
	WDFDRIVER driver;
	NTSTATUS status;

	WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
	config.EvtDriverUnload = EvtUnload;

	status = WdfDriverCreate(DriverObject, RegistryPath,
		WDF_NO_OBJECT_ATTRIBUTES, &config, &driver);
	if (!NT_SUCCESS(status))
		return status;

	status = CreateControlDevice(driver);
	if (!NT_SUCCESS(status))
		return status;

	g_BootDiskNumber = GetBootDiskNumber();
	DbgPrint("go2cloud_flt: boot disk = %u\n", g_BootDiskNumber);

	DbgPrint("go2cloud_flt: loaded v%d.%d.%d\n",
		DCBT_VERSION_MAJOR, DCBT_VERSION_MINOR, DCBT_VERSION_PATCH);

	return STATUS_SUCCESS;
}
