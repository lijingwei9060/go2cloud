/*
 * device.h — internal structures shared across driver source files
 */

#ifndef DRIVER_DEVICE_H
#define DRIVER_DEVICE_H

#include <ntifs.h>
#include <wdf.h>
#include "dcbt_ioctl.h"

/* Per-disk filter device context */
typedef struct _FILTER_DEVICE_CONTEXT {
    ULONG   DiskNumber;       /* PhysicalDriveN index */
    LONGLONG TotalBytes;     /* disk size in bytes */
    ULONG   TotalBlocks;     /* total 1MB blocks */
    ULONG   BitmapSize;      /* bitmap byte count */

    PUCHAR  Bitmap;          /* NonPagedPool — dirty-block bitmap */
    WDFSPINLOCK BitmapLock;  /* protects Bitmap + DirtyCount + stats */

    ULONG   DirtyCount;      /* current dirty block count */

    /* Statistics */
    ULONG64 TotalWrites;
    ULONG64 TotalBytesWritten;
    LARGE_INTEGER FirstWriteTick;
    LARGE_INTEGER LastWriteTick;
} FILTER_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_DEVICE_CONTEXT, FilterGetContext);

/* Control device context — owns the global disk table */
typedef struct _CONTROL_DEVICE_CONTEXT {
    FILTER_DEVICE_CONTEXT *DiskTable[DCBT_MAX_DISKS];
    WDFSPINLOCK TableLock;  /* protects DiskTable[] */
} CONTROL_DEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CONTROL_DEVICE_CONTEXT, ControlGetContext);

/* Function declarations */
NTSTATUS BitmapInit(FILTER_DEVICE_CONTEXT *ctx, ULONG disk_number, LONGLONG total_bytes);
VOID BitmapFree(FILTER_DEVICE_CONTEXT *ctx);
VOID FilterRegisterDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx, ULONG disk_number, FILTER_DEVICE_CONTEXT *ctx);
VOID FilterUnregisterDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx, ULONG disk_number);
FILTER_DEVICE_CONTEXT *FilterLookupDisk(CONTROL_DEVICE_CONTEXT *ctrl_ctx, ULONG disk_number);

/* Bitmap operations */
VOID BitmapMark(FILTER_DEVICE_CONTEXT *ctx, ULONG start_block, ULONG end_block);
VOID BitmapClear(FILTER_DEVICE_CONTEXT *ctx);
ULONG BitmapCountDirty(FILTER_DEVICE_CONTEXT *ctx);

/* Dispatch callbacks (defined in dispatch.c) */
NTSTATUS EvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                            size_t OutputBufferLength, size_t InputBufferLength,
                            ULONG IoControlCode);
NTSTATUS EvtIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length);
VOID EvtIoDefault(WDFQUEUE Queue, WDFREQUEST Request);

/* IOCTL handlers */
NTSTATUS IoctlGetBitmap(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request);
NTSTATUS IoctlClearBitmap(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request);
NTSTATUS IoctlGetStats(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request);
NTSTATUS IoctlGetVersion(WDFREQUEST Request);
NTSTATUS IoctlQueryBitmapSize(CONTROL_DEVICE_CONTEXT *ctrl_ctx, WDFREQUEST Request);

#endif /* DRIVER_DEVICE_H */
