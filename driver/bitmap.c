/*
 * bitmap.c — dirty-block bitmap operations
 *
 * All functions assume the caller holds BitmapLock unless noted otherwise.
 */

#include "device.h"
#include "bitmap.h"

#pragma warning(disable: 4054)  /* cast function pointer to PVOID */
#pragma warning(disable: 4055)  /* cast function pointer to PVOID */

/* Allocate and initialize the bitmap for a disk.
 * IMPORTANT: ctx->Bitmap is set LAST so that EvtIoWrite (which checks
 * Bitmap under BitmapLock to decide whether to track) never sees a
 * non-NULL pointer before TotalBlocks / BitmapSize are valid. */
NTSTATUS BitmapInit(FILTER_DEVICE_CONTEXT *ctx, ULONG disk_number, LONGLONG total_bytes)
{
	ULONG blocks = TOTAL_BLOCKS_FROM_DISK(total_bytes);
	ULONG size = (blocks + 7) / 8;
	PUCHAR bm;

	if (blocks == 0 || size == 0)
		return STATUS_INVALID_PARAMETER;

	bm = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'T2CG');
	if (!bm)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(bm, size);

	/* Set all metadata before publishing the bitmap pointer */
	ctx->DiskNumber = disk_number;
	ctx->TotalBytes = total_bytes;
	ctx->TotalBlocks = blocks;
	ctx->BitmapSize = size;
	ctx->DirtyCount = 0;
	ctx->TotalWrites = 0;
	ctx->TotalBytesWritten = 0;
	ctx->FirstWriteTick.QuadPart = 0;
	ctx->LastWriteTick.QuadPart = 0;

	/* Full memory barrier: metadata stores MUST be globally visible
	 * before Bitmap store, on all architectures including ARM64.
	 * _WriteBarrier() is compiler-only; KeMemoryBarrier() also prevents
	 * CPU store reordering on weakly-ordered architectures. */
	KeMemoryBarrier();
	ctx->Bitmap = bm;

	return STATUS_SUCCESS;
}

/* Free the bitmap */
VOID BitmapFree(FILTER_DEVICE_CONTEXT *ctx)
{
	if (ctx->Bitmap) {
		ExFreePool(ctx->Bitmap);
		ctx->Bitmap = NULL;
	}
	ctx->BitmapSize = 0;
	ctx->TotalBlocks = 0;
	ctx->TotalBytes = 0;
	ctx->DirtyCount = 0;
	ctx->TotalWrites = 0;
	ctx->TotalBytesWritten = 0;
	ctx->FirstWriteTick.QuadPart = 0;
	ctx->LastWriteTick.QuadPart = 0;
}

/* Mark blocks [start, end] as dirty. Caller holds BitmapLock. */
VOID BitmapMark(FILTER_DEVICE_CONTEXT *ctx, ULONG start, ULONG end)
{
	PUCHAR bm = ctx->Bitmap;
	ULONG count = 0;
	ULONG max = ctx->TotalBlocks;

	if (end >= max)
		end = max - 1;
	if (start > end || start >= max)
		return;

	for (ULONG i = start; i <= end; i++) {
		if (!BITMAP_TEST(bm, i)) {
			BITMAP_SET(bm, i);
			count++;
		}
	}
	ctx->DirtyCount += count;
}

/* Clear entire bitmap. Caller holds BitmapLock. */
VOID BitmapClear(FILTER_DEVICE_CONTEXT *ctx)
{
	BITMAP_ZERO(ctx->Bitmap, ctx->BitmapSize);
	ctx->DirtyCount = 0;
}

/* Return dirty block count. Caller holds BitmapLock (or reads atomically). */
ULONG BitmapCountDirty(FILTER_DEVICE_CONTEXT *ctx)
{
	return ctx->DirtyCount;
}
