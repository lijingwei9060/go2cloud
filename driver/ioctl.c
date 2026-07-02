/*
 * ioctl.c — IOCTL dispatch handlers for the control device
 */

#include "device.h"
#include "bitmap.h"

#pragma warning(disable: 4054)
#pragma warning(disable: 4055)

/*
 * IOCTL_GO2CLOUD_GET_BITMAP
 *   Input:  ULONG DiskNumber
 *   Output: GO2CLOUD_BITMAP (variable-length)
 *
 * Two-phase locking: first capture BitmapSize under BitmapLock, then release
 * and retrieve the output buffer via WDF calls outside the lock.  Re-acquire
 * for the metadata+bitmap snapshot so the data is consistent.
 */
NTSTATUS IoctlGetBitmap(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request)
{
	NTSTATUS status;
	FILTER_DEVICE_CONTEXT *ctx;
	WDFMEMORY out_mem = WDF_NO_HANDLE;
	PVOID in_buf, out_buf;
	size_t in_size, out_size, needed;

	/* --- read DiskNumber from input (no lock needed) --- */
	{
		WDFMEMORY in_mem = WDF_NO_HANDLE;
		status = WdfRequestRetrieveInputMemory(Request, &in_mem);
		if (!NT_SUCCESS(status))
			return status;
		in_buf = WdfMemoryGetBuffer(in_mem, &in_size);
		if (!in_buf || in_size < sizeof(ULONG))
			return STATUS_BUFFER_TOO_SMALL;
	}

	ULONG disk_number = *(PULONG)in_buf;

	/* Phase 1: capture bitmap size under lock, then release */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;

	needed = FIELD_OFFSET(GO2CLOUD_BITMAP, Bitmap) + ctx->BitmapSize;
	FilterLookupRelease(ctx);

	/* Phase 2: framework calls — no lock held */
	status = WdfRequestRetrieveOutputMemory(Request, &out_mem);
	if (!NT_SUCCESS(status))
		return status;

	out_buf = WdfMemoryGetBuffer(out_mem, &out_size);
	if (!out_buf || out_size < needed)
		return STATUS_BUFFER_TOO_SMALL;

	/* Phase 3: re-acquire lock for a consistent snapshot */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;

	PGO2CLOUD_BITMAP out = (PGO2CLOUD_BITMAP)out_buf;
	out->DiskNumber = ctx->DiskNumber;
	out->TotalBytes = ctx->TotalBytes;
	out->BlockSize = DCBT_BLOCK_SIZE;
	out->TotalBlocks = ctx->TotalBlocks;
	out->DirtyBlocks = ctx->DirtyCount;
	out->BitmapSize = ctx->BitmapSize;
	RtlCopyMemory(out->Bitmap, ctx->Bitmap, ctx->BitmapSize);

	FilterLookupRelease(ctx);

	WdfRequestSetInformation(Request, needed);
	return STATUS_SUCCESS;
}

/*
 * IOCTL_GO2CLOUD_CLEAR_BITMAP
 *   Input: ULONG DiskNumber
 */
NTSTATUS IoctlClearBitmap(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request)
{
	WDFMEMORY in_mem = WDF_NO_HANDLE;
	PVOID in_buf;
	size_t in_size;
	FILTER_DEVICE_CONTEXT *ctx;
	NTSTATUS status;

	status = WdfRequestRetrieveInputMemory(Request, &in_mem);
	if (!NT_SUCCESS(status))
		return status;

	in_buf = WdfMemoryGetBuffer(in_mem, &in_size);
	if (!in_buf || in_size < sizeof(ULONG))
		return STATUS_BUFFER_TOO_SMALL;

	ULONG disk_number = *(PULONG)in_buf;

	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;

	BitmapClear(ctx);
	FilterLookupRelease(ctx);

	return STATUS_SUCCESS;
}

/*
 * IOCTL_GO2CLOUD_GET_STATS
 *   Input:  ULONG DiskNumber
 *   Output: GO2CLOUD_STATS
 */
NTSTATUS IoctlGetStats(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request)
{
	WDFMEMORY in_mem = WDF_NO_HANDLE, out_mem = WDF_NO_HANDLE;
	PVOID in_buf, out_buf;
	size_t in_size, out_size;
	FILTER_DEVICE_CONTEXT *ctx;
	NTSTATUS status;

	status = WdfRequestRetrieveInputMemory(Request, &in_mem);
	if (!NT_SUCCESS(status))
		return status;

	in_buf = WdfMemoryGetBuffer(in_mem, &in_size);
	if (!in_buf || in_size < sizeof(ULONG))
		return STATUS_BUFFER_TOO_SMALL;

	ULONG disk_number = *(PULONG)in_buf;

	/* Phase 1: locked — validate disk and capture stats size req */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;
	FilterLookupRelease(ctx);

	/* Phase 2: framework call — no lock */
	status = WdfRequestRetrieveOutputMemory(Request, &out_mem);
	if (!NT_SUCCESS(status))
		return status;

	out_buf = WdfMemoryGetBuffer(out_mem, &out_size);
	if (!out_buf || out_size < sizeof(GO2CLOUD_STATS))
		return STATUS_BUFFER_TOO_SMALL;

	/* Phase 3: re-acquire for consistent snapshot */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;

	PGO2CLOUD_STATS out = (PGO2CLOUD_STATS)out_buf;
	out->DiskNumber = ctx->DiskNumber;
	out->TotalWrites = ctx->TotalWrites;
	out->TotalBytesWritten = ctx->TotalBytesWritten;
	out->FirstWriteTick = ctx->FirstWriteTick.QuadPart;
	out->LastWriteTick = ctx->LastWriteTick.QuadPart;

	FilterLookupRelease(ctx);

	WdfRequestSetInformation(Request, sizeof(GO2CLOUD_STATS));
	return STATUS_SUCCESS;
}

/*
 * IOCTL_GO2CLOUD_GET_VERSION
 *   Output: GO2CLOUD_VERSION
 */
NTSTATUS IoctlGetVersion(WDFREQUEST Request)
{
	WDFMEMORY out_mem = WDF_NO_HANDLE;
	PVOID out_buf;
	size_t out_size;
	NTSTATUS status;

	status = WdfRequestRetrieveOutputMemory(Request, &out_mem);
	if (!NT_SUCCESS(status))
		return status;

	out_buf = WdfMemoryGetBuffer(out_mem, &out_size);
	if (!out_buf || out_size < sizeof(GO2CLOUD_VERSION))
		return STATUS_BUFFER_TOO_SMALL;

	PGO2CLOUD_VERSION out = (PGO2CLOUD_VERSION)out_buf;
	out->Major = DCBT_VERSION_MAJOR;
	out->Minor = DCBT_VERSION_MINOR;
	out->Patch = DCBT_VERSION_PATCH;

	WdfRequestSetInformation(Request, sizeof(GO2CLOUD_VERSION));
	return STATUS_SUCCESS;
}

/*
 * IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE
 *   Input:  ULONG DiskNumber
 *   Output: GO2CLOUD_BITMAP_SIZE
 */
NTSTATUS IoctlQueryBitmapSize(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request)
{
	WDFMEMORY in_mem = WDF_NO_HANDLE, out_mem = WDF_NO_HANDLE;
	PVOID in_buf, out_buf;
	size_t in_size, out_size;
	FILTER_DEVICE_CONTEXT *ctx;
	NTSTATUS status;

	status = WdfRequestRetrieveInputMemory(Request, &in_mem);
	if (!NT_SUCCESS(status))
		return status;

	in_buf = WdfMemoryGetBuffer(in_mem, &in_size);
	if (!in_buf || in_size < sizeof(ULONG))
		return STATUS_BUFFER_TOO_SMALL;

	ULONG disk_number = *(PULONG)in_buf;

	/* Phase 1: locked — validate disk presence */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;
	FilterLookupRelease(ctx);

	/* Phase 2: framework call — no lock */
	status = WdfRequestRetrieveOutputMemory(Request, &out_mem);
	if (!NT_SUCCESS(status))
		return status;

	out_buf = WdfMemoryGetBuffer(out_mem, &out_size);
	if (!out_buf || out_size < sizeof(GO2CLOUD_BITMAP_SIZE))
		return STATUS_BUFFER_TOO_SMALL;

	/* Phase 3: re-acquire for consistent read */
	ctx = FilterLookupDisk(ctrl_ctx, disk_number);
	if (!ctx)
		return STATUS_NOT_FOUND;

	PGO2CLOUD_BITMAP_SIZE out = (PGO2CLOUD_BITMAP_SIZE)out_buf;
	out->DiskNumber = ctx->DiskNumber;
	out->TotalBlocks = ctx->TotalBlocks;
	out->BitmapSize = ctx->BitmapSize;

	FilterLookupRelease(ctx);

	WdfRequestSetInformation(Request, sizeof(GO2CLOUD_BITMAP_SIZE));
	return STATUS_SUCCESS;
}
