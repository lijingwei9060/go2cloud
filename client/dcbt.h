/*
 * dcbt.h — user-mode DCBT (Driver Change Block Tracking) API
 *
 * Communicates with the go2cloud_flt.sys kernel driver to query
 * and clear dirty-block bitmaps for incremental disk sync.
 */

#ifndef CLIENT_DCBT_H
#define CLIENT_DCBT_H

#include <windows.h>
#include "dcbt_ioctl.h"

#define DCBT_MAX_DEVICE_PATH  256

typedef struct {
	HANDLE    hDevice;
	int       available;       /* 1 = driver opened successfully */
} dcbt_t;

/* Open \\.\go2cloud_flt control device. Returns 0 on success. */
int dcbt_open(dcbt_t *dcbt);

/* Close the device handle. */
void dcbt_close(dcbt_t *dcbt);

/* Non-zero if the DCBT driver is loaded and accessible. */
int dcbt_is_available(const dcbt_t *dcbt);

/*
 * Query the bitmap size for a given disk.
 * The caller uses bitmap_size to allocate the buffer for dcbt_get_bitmap().
 * Returns 0 on success.
 */
int dcbt_query_bitmap_size(dcbt_t *dcbt, ULONG disk_number,
                           ULONG *total_blocks, ULONG *bitmap_size);

/*
 * Get the dirty-block bitmap for a disk.
 * buf must be at least bitmap_size bytes (from dcbt_query_bitmap_size).
 * On success, *dirty_blocks is set to the count of dirty blocks.
 * Returns 0 on success.
 */
int dcbt_get_bitmap(dcbt_t *dcbt, ULONG disk_number,
                    void *buf, ULONG buf_size, ULONG *dirty_blocks);

/*
 * Clear the dirty bitmap for a disk.
 * Call after the incremental round has been ACK'd by the server.
 * Returns 0 on success.
 */
int dcbt_clear_bitmap(dcbt_t *dcbt, ULONG disk_number);

/*
 * Get tracking statistics for a disk.
 * Returns 0 on success.
 */
int dcbt_get_stats(dcbt_t *dcbt, ULONG disk_number, GO2CLOUD_STATS *stats);

/*
 * Test whether a specific 1 MB block is marked dirty in the bitmap.
 * bitmap: raw bitmap bytes from dcbt_get_bitmap()
 * block_index: 0-based block number (offset / 1MB)
 */
static inline int
dcbt_is_block_dirty(const unsigned char *bitmap, ULONG block_index)
{
	return (bitmap[block_index >> 3] >> (block_index & 7)) & 1;
}

#endif /* CLIENT_DCBT_H */
