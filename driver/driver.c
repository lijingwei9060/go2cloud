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

/* Forward declarations for PnP callbacks */
static NTSTATUS EvtDevicePrepareHardware(WDFDEVICE, WDFCMRESLIST, WDFCMRESLIST);
static NTSTATUS EvtDeviceReleaseHardware(WDFDEVICE, WDFCMRESLIST);

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

FILTER_DEVICE_CONTEXT *FilterLookupDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx,
                                        ULONG disk_number)
{
	if (!ctrl_ctx || disk_number >= DCBT_MAX_DISKS)
		return NULL;

	WdfSpinLockAcquire(ctrl_ctx->TableLock);
	FILTER_DEVICE_CONTEXT *ctx = ctrl_ctx->DiskTable[disk_number];
	WdfSpinLockRelease(ctrl_ctx->TableLock);

	return ctx;
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
		return status;

	status = WdfDeviceCreateSymbolicLink(ctrl_dev, &sym_name);
	if (!NT_SUCCESS(status))
		return status;

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_cfg, WdfIoQueueDispatchSequential);
	queue_cfg.EvtIoDeviceControl = EvtIoDeviceControl;

	status = WdfIoQueueCreate(ctrl_dev, &queue_cfg,
		WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
	if (!NT_SUCCESS(status))
		return status;

	g_CtrlCtx = ControlGetContext(ctrl_dev);
	RtlZeroMemory(g_CtrlCtx->DiskTable, sizeof(g_CtrlCtx->DiskTable));

	status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &g_CtrlCtx->TableLock);
	if (!NT_SUCCESS(status))
		return status;

	WdfControlFinishInitializing(ctrl_dev);
	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * EvtDriverUnload
 * --------------------------------------------------------------- */

static VOID EvtUnload(WDFDRIVER Driver)
{
	UNREFERENCED_PARAMETER(Driver);

	if (g_CtrlCtx) {
		for (ULONG i = 0; i < DCBT_MAX_DISKS; i++) {
			if (g_CtrlCtx->DiskTable[i]) {
				BitmapFree(g_CtrlCtx->DiskTable[i]);
				g_CtrlCtx->DiskTable[i] = NULL;
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
 * EvtDevicePrepareHardware — disk powered on; query number + size
 * --------------------------------------------------------------- */

static NTSTATUS
EvtDevicePrepareHardware(WDFDEVICE Device,
                         WDFCMRESLIST ResourcesRaw,
                         WDFCMRESLIST ResourcesTranslated)
{
	UNREFERENCED_PARAMETER(ResourcesRaw);
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(Device);
	WDFIOTARGET target = WdfDeviceGetIoTarget(Device);
	WDF_MEMORY_DESCRIPTOR out_desc;
	NTSTATUS status;

	/* Query PhysicalDriveN number */
	STORAGE_DEVICE_NUMBER sd = {0};
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc, &sd, sizeof(sd));

	status = WdfIoTargetSendIoctlSynchronously(
		target, WDF_NO_HANDLE,
		IOCTL_STORAGE_GET_DEVICE_NUMBER,
		NULL, &out_desc, NULL, NULL);

	if (!NT_SUCCESS(status))
		return STATUS_SUCCESS;

	/* Query disk length */
	GET_LENGTH_INFORMATION len_info = {0};
	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out_desc, &len_info, sizeof(len_info));

	status = WdfIoTargetSendIoctlSynchronously(
		target, WDF_NO_HANDLE,
		IOCTL_DISK_GET_LENGTH_INFO,
		NULL, &out_desc, NULL, NULL);

	if (!NT_SUCCESS(status))
		return STATUS_SUCCESS;

	/* Allocate bitmap and register */
	ctx->DiskNumber = sd.DeviceNumber;

	status = BitmapInit(ctx, ctx->DiskNumber, len_info.Length.QuadPart);
	if (!NT_SUCCESS(status))
		return STATUS_SUCCESS;

	if (g_CtrlCtx)
		FilterRegisterDisk(g_CtrlCtx, ctx->DiskNumber, ctx);

	DbgPrint("go2cloud_flt: Disk%u ready — %u blocks, %u byte bitmap\n",
		ctx->DiskNumber, ctx->TotalBlocks, ctx->BitmapSize);

	return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------
 * EvtDeviceReleaseHardware — disk going away; free bitmap
 * --------------------------------------------------------------- */

static NTSTATUS
EvtDeviceReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(Device);

	if (ctx->Bitmap && ctx->DiskNumber != (ULONG)-1) {
		if (g_CtrlCtx)
			FilterUnregisterDisk(g_CtrlCtx, ctx->DiskNumber);
		BitmapFree(ctx);
	}

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

	DbgPrint("go2cloud_flt: loaded v%d.%d.%d\n",
		DCBT_VERSION_MAJOR, DCBT_VERSION_MINOR, DCBT_VERSION_PATCH);

	return STATUS_SUCCESS;
}
