/*
 * bitmap.h — dirty-block bitmap internal macros
 *
 * Each block is 1 MB. Each bit represents one block.
 * Bit = 1 → block is dirty (has been written to).
 */

#ifndef DRIVER_BITMAP_H
#define DRIVER_BITMAP_H

#include "device.h"

/* Calculate bitmap byte size from total disk bytes */
#define BITMAP_BYTES_FROM_DISK(tot) \
    ((ULONG)((((ULONG64)(tot) + DCBT_BLOCK_SIZE - 1) / DCBT_BLOCK_SIZE + 7) / 8))

/* Calculate total blocks from total disk bytes */
#define TOTAL_BLOCKS_FROM_DISK(tot) \
    ((ULONG)(((ULONG64)(tot) + DCBT_BLOCK_SIZE - 1) / DCBT_BLOCK_SIZE))

/* Set bit at block index i (caller holds lock) */
#define BITMAP_SET(bm, i) \
    ((bm)[(i) >> 3] |= (1 << ((i) & 7)))

/* Test bit at block index i (caller holds lock) */
#define BITMAP_TEST(bm, i) \
    (((bm)[(i) >> 3] >> ((i) & 7)) & 1)

/* Clear entire bitmap (caller holds lock) */
#define BITMAP_ZERO(bm, sz) \
    RtlZeroMemory((bm), (sz))

#endif /* DRIVER_BITMAP_H */
