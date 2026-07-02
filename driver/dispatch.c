/*
 * dispatch.c — IRP dispatch callbacks
 *
 * Filter device (one per disk):
 *   EvtIoWrite  — track writes via WDM IRP, then pass down
 *   EvtIoDefault — pass everything else through
 *
 * Control device (singleton):
 *   EvtIoDeviceControl — route IOCTLs to ioctl.c handlers
 *
 * All EvtIo* callbacks return VOID per the WDF contract.
 * Requests are completed explicitly via WdfRequestComplete or implicitly
 * by WdfRequestSend with SEND_AND_FORGET.
 */

#include "device.h"
#include "bitmap.h"

#pragma warning(disable: 4054)
#pragma warning(disable: 4055)

extern CONTROL_DEVICE_CONTEXT *g_CtrlCtx;

/* ---------------------------------------------------------------
 * Filter device — write tracking
 * --------------------------------------------------------------- */

VOID
EvtIoWrite(WDFQUEUE Queue, WDFREQUEST Request, size_t Length)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Length);

	PIRP irp = WdfRequestWdmGetIrp(Request);
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(irp);
	LONGLONG offset = stack->Parameters.Write.ByteOffset.QuadPart;
	ULONG write_len = stack->Parameters.Write.Length;

	WDFDEVICE device = WdfIoQueueGetDevice(Queue);
	FILTER_DEVICE_CONTEXT *ctx = FilterGetContext(device);

	WdfSpinLockAcquire(ctx->BitmapLock);

	if (write_len > 0 && ctx->Bitmap) {
		ULONG start = (ULONG)((ULONG64)offset / DCBT_BLOCK_SIZE);
		ULONG end   = (ULONG)(((ULONG64)offset + write_len - 1) / DCBT_BLOCK_SIZE);
		LARGE_INTEGER tick;

		KeQueryTickCount(&tick);

		BitmapMark(ctx, start, end);
		ctx->TotalWrites++;
		ctx->TotalBytesWritten += (ULONG64)write_len;
		if (ctx->FirstWriteTick.QuadPart == 0)
			ctx->FirstWriteTick = tick;
		ctx->LastWriteTick = tick;
	}

	WdfSpinLockRelease(ctx->BitmapLock);

	WDF_REQUEST_SEND_OPTIONS options;
	WDF_REQUEST_SEND_OPTIONS_INIT(&options,
		WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options)) {
		WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
	}
}

/* ---------------------------------------------------------------
 * Filter device — pass-through for all other IRPs
 * --------------------------------------------------------------- */

VOID
EvtIoDefault(WDFQUEUE Queue, WDFREQUEST Request)
{
	WDF_REQUEST_SEND_OPTIONS options;
	WDF_REQUEST_SEND_OPTIONS_INIT(&options,
		WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	if (!WdfRequestSend(Request,
		WdfDeviceGetIoTarget(WdfIoQueueGetDevice(Queue)),
		&options)) {
		WdfRequestComplete(Request, STATUS_UNSUCCESSFUL);
	}
}

/* ---------------------------------------------------------------
 * Control device — IOCTL dispatch
 * --------------------------------------------------------------- */

VOID
EvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
                   size_t OutputBufferLength, size_t InputBufferLength,
                   ULONG IoControlCode)
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	NTSTATUS status = STATUS_NOT_SUPPORTED;

	switch (IoControlCode) {
	case IOCTL_GO2CLOUD_GET_BITMAP:
		status = IoctlGetBitmap(g_CtrlCtx, Request);
		break;
	case IOCTL_GO2CLOUD_CLEAR_BITMAP:
		status = IoctlClearBitmap(g_CtrlCtx, Request);
		break;
	case IOCTL_GO2CLOUD_GET_STATS:
		status = IoctlGetStats(g_CtrlCtx, Request);
		break;
	case IOCTL_GO2CLOUD_GET_VERSION:
		status = IoctlGetVersion(Request);
		break;
	case IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE:
		status = IoctlQueryBitmapSize(g_CtrlCtx, Request);
		break;
	}

	WdfRequestComplete(Request, status);
}
