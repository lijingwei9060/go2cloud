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
 * Callable with or without BitmapLock — store-release ordering
 * (ctx->Bitmap set last, after KeMemoryBarrier) ensures readers
 * under BitmapLock see either NULL or a fully-initialized bitmap.
 * Callers that hold BitmapLock (e.g. work item Phase 2) get
 * stronger ordering guarantees; callers that don't rely on the
 * release barrier to publish atomically to EvtIoWrite. */
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

/*
 * Popcount of a byte — how many bits are set.  Indexed by byte value.
 * 8 - g_Popcount[b] gives the number of zero bits in byte b.
 */
static const UCHAR g_Popcount[256] = {
	0,1,1,2,1,2,2,3,1,2,2,3,2,3,3,4,  /* 0x00-0x0F */
	1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,  /* 0x10-0x1F */
	1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,  /* 0x20-0x2F */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0x30-0x3F */
	1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,  /* 0x40-0x4F */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0x50-0x5F */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0x60-0x6F */
	3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,  /* 0x70-0x7F */
	1,2,2,3,2,3,3,4,2,3,3,4,3,4,4,5,  /* 0x80-0x8F */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0x90-0x9F */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0xA0-0xAF */
	3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,  /* 0xB0-0xBF */
	2,3,3,4,3,4,4,5,3,4,4,5,4,5,5,6,  /* 0xC0-0xCF */
	3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,  /* 0xD0-0xDF */
	3,4,4,5,4,5,5,6,4,5,5,6,5,6,6,7,  /* 0xE0-0xEF */
	4,5,5,6,5,6,6,7,5,6,6,7,6,7,7,8,  /* 0xF0-0xFF */
};

/* Mark blocks [start, end] as dirty. Caller holds BitmapLock.
 * Uses byte-level bulk operations for interior bytes so that large
 * writes (e.g. 1 GB = 1024 blocks = 128 bytes) complete in O(bytes)
 * instead of O(blocks) while holding the spinlock. */
VOID BitmapMark(FILTER_DEVICE_CONTEXT *ctx, ULONG start, ULONG end)
{
	PUCHAR bm = ctx->Bitmap;
	ULONG max = ctx->TotalBlocks;
	ULONG count = 0;

	if (end >= max)
		end = max - 1;
	if (start > end || start >= max)
		return;

	ULONG byte_start = start / 8;
	ULONG byte_end   = end / 8;
	ULONG bit_start  = start % 8;
	ULONG bit_end    = end % 8;

	if (byte_start == byte_end) {
		/* All bits within a single byte */
		UCHAR mask = (UCHAR)((0xFFU << bit_start) &
		                     (0xFFU >> (7 - bit_end)));
		UCHAR old = bm[byte_start];
		UCHAR added = mask & ~old;
		bm[byte_start] = old | mask;
		count = g_Popcount[added];
	} else {
		/* First partial byte: bits [bit_start, 7] */
		if (bit_start != 0) {
			UCHAR mask = (UCHAR)(0xFFU << bit_start);
			UCHAR old = bm[byte_start];
			UCHAR added = mask & ~old;
			bm[byte_start] = old | mask;
			count += g_Popcount[added];
			byte_start++;
		}

		/* Last partial byte: bits [0, bit_end] */
		if (bit_end != 7) {
			UCHAR mask = (UCHAR)(0xFFU >> (7 - bit_end));
			UCHAR old = bm[byte_end];
			UCHAR added = mask & ~old;
			bm[byte_end] = old | mask;
			count += g_Popcount[added];
			byte_end--;
		}

		/* Full interior bytes — bulk set to 0xFF, skip already-dirty */
		for (ULONG i = byte_start; i <= byte_end; i++) {
			UCHAR old = bm[i];
			if (old != 0xFF) {
				bm[i] = 0xFF;
				count += 8 - g_Popcount[old];
			}
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
