/*
 * dcbt.c — user-mode DCBT interface via IOCTL to go2cloud_flt.sys
 *
 * Opens \\.\go2cloud_flt (the control device created by the kernel driver)
 * and sends buffered IOCTLs to query/clear per-disk dirty-block bitmaps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dcbt.h"
#include "log.h"

#define DCBT_DEVICE_PATH  "\\\\.\\go2cloud_flt"

/* Open the DCBT control device. */
int
dcbt_open(dcbt_t *dcbt)
{
	if (!dcbt) return -1;

	memset(dcbt, 0, sizeof(*dcbt));

	dcbt->hDevice = CreateFileA(
		DCBT_DEVICE_PATH,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (dcbt->hDevice == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		LOG_INFO("dcbt: driver not available (%s, err=%lu)",
		         DCBT_DEVICE_PATH, err);
		dcbt->hDevice = NULL;
		dcbt->available = 0;
		return -1;
	}

	dcbt->available = 1;
	LOG_INFO("dcbt: driver opened (%s)", DCBT_DEVICE_PATH);
	return 0;
}

/* Close the device. */
void
dcbt_close(dcbt_t *dcbt)
{
	if (dcbt && dcbt->hDevice) {
		CloseHandle(dcbt->hDevice);
		dcbt->hDevice = NULL;
	}
	dcbt->available = 0;
}

int dcbt_is_available(const dcbt_t *dcbt)
{
	return dcbt && dcbt->available;
}

/*
 * Send an IOCTL with a ULONG input (disk number) and a variable-size output.
 * Returns output buffer via DeviceIoControl, or -1 on error.
 */
static int
dcbt_ioctl_in_ulong_out_buf(dcbt_t *dcbt, DWORD ioctl_code,
                            ULONG disk_number,
                            void *out_buf, DWORD out_buf_size,
                            DWORD *bytes_returned)
{
	DWORD bytes = 0;

	if (!dcbt || !dcbt->hDevice || !out_buf)
		return -1;

	if (!DeviceIoControl(dcbt->hDevice, ioctl_code,
	                     &disk_number, sizeof(disk_number),
	                     out_buf, out_buf_size,
	                     &bytes, NULL)) {
		DWORD err = GetLastError();
		LOG_DEBUG("dcbt: IOCTL 0x%08x disk=%lu failed, err=%lu",
		          ioctl_code, disk_number, err);
		return -1;
	}

	if (bytes_returned)
		*bytes_returned = bytes;
	return 0;
}

/* Query the bitmap size for a disk. */
int
dcbt_query_bitmap_size(dcbt_t *dcbt, ULONG disk_number,
                       ULONG *total_blocks, ULONG *bitmap_size)
{
	GO2CLOUD_BITMAP_SIZE info = {0};

	if (dcbt_ioctl_in_ulong_out_buf(dcbt, IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE,
	                                disk_number, &info, sizeof(info), NULL) != 0)
		return -1;

	if (total_blocks) *total_blocks = info.TotalBlocks;
	if (bitmap_size)  *bitmap_size  = info.BitmapSize;
	return 0;
}

/* Get the dirty-block bitmap for a disk. */
int
dcbt_get_bitmap(dcbt_t *dcbt, ULONG disk_number,
                void *buf, ULONG buf_size, ULONG *dirty_blocks)
{
	DWORD bytes_returned = 0;

	if (!buf || buf_size < sizeof(GO2CLOUD_BITMAP))
		return -1;

	/*
	 * The kernel returns a variable-length GO2CLOUD_BITMAP struct.
	 * buf must be large enough to hold the header + Bitmap[] payload.
	 */
	if (!DeviceIoControl(dcbt->hDevice, IOCTL_GO2CLOUD_GET_BITMAP,
	                     &disk_number, sizeof(disk_number),
	                     buf, buf_size,
	                     &bytes_returned, NULL)) {
		DWORD err = GetLastError();
		LOG_DEBUG("dcbt: GET_BITMAP disk=%lu failed, err=%lu",
		          disk_number, err);
		return -1;
	}

	if (bytes_returned < FIELD_OFFSET(GO2CLOUD_BITMAP, Bitmap)) {
		LOG_DEBUG("dcbt: GET_BITMAP disk=%lu short read (%lu bytes)",
		          disk_number, bytes_returned);
		return -1;
	}

	GO2CLOUD_BITMAP *hdr = (GO2CLOUD_BITMAP *)buf;
	if (dirty_blocks)
		*dirty_blocks = hdr->DirtyBlocks;

	return 0;
}

/* Clear the dirty bitmap for a disk. */
int
dcbt_clear_bitmap(dcbt_t *dcbt, ULONG disk_number)
{
	if (!dcbt || !dcbt->hDevice)
		return -1;

	DWORD bytes = 0;
	if (!DeviceIoControl(dcbt->hDevice, IOCTL_GO2CLOUD_CLEAR_BITMAP,
	                     &disk_number, sizeof(disk_number),
	                     NULL, 0,
	                     &bytes, NULL)) {
		DWORD err = GetLastError();
		LOG_DEBUG("dcbt: CLEAR_BITMAP disk=%lu failed, err=%lu",
		          disk_number, err);
		return -1;
	}

	LOG_DEBUG("dcbt: cleared bitmap for disk %lu", disk_number);
	return 0;
}

/* Get stats for a disk. */
int
dcbt_get_stats(dcbt_t *dcbt, ULONG disk_number, GO2CLOUD_STATS *stats)
{
	return dcbt_ioctl_in_ulong_out_buf(dcbt, IOCTL_GO2CLOUD_GET_STATS,
	                                   disk_number, stats, sizeof(*stats), NULL);
}
