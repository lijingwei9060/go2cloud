/*
 * block_writer.c — target disk block write module implementation
 *
 * Implements lseek + write loop with retry for complete block writes.
 */

#include "block_writer.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#define lseek64  _lseeki64
#define write    _write
#define fsync    _commit
#define open     _open
#define close    _close
#define O_RDWR   _O_RDWR
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#else
#include <fcntl.h>
#include <unistd.h>
#define lseek64  lseek
#endif

/* Target disk descriptor */
typedef struct {
    int   fd;           /* open file descriptor, -1 = not open */
    char  path[512];    /* file path */
    int   registered;   /* 1 = registered */
} target_disk_t;

/* Periodic fsync: flush every N blocks (~= every N MB) */
#define FSYNC_BLOCK_INTERVAL  2000

static target_disk_t g_disks[MAX_TARGET_DISKS];
static uint64_t      g_total_blocks = 0;
static uint64_t      g_total_bytes  = 0;
static uint64_t      g_blocks_since_fsync = 0;
static uint64_t      g_pending_bytes = 0;  /* bytes since last BINLOG notification */

int block_writer_init(void) {
    memset(g_disks, 0, sizeof(g_disks));
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        g_disks[i].fd = -1;
    }
    g_total_blocks = 0;
    g_total_bytes  = 0;
    g_blocks_since_fsync = 0;
    return 0;
}

int block_writer_register(int devno, const char *filepath) {
    if (devno < 0 || devno >= MAX_TARGET_DISKS) {
        LOG_ERROR("devno %d out of range [0, %d)", devno, MAX_TARGET_DISKS);
        return -1;
    }
    snprintf(g_disks[devno].path, sizeof(g_disks[devno].path),
             "%s", filepath);
    g_disks[devno].registered = 1;
    LOG_INFO("registered disk %d -> %s", devno, filepath);
    return 0;
}

int block_writer_open_all(void) {
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        if (!g_disks[i].registered) continue;

        g_disks[i].fd = open(g_disks[i].path, O_RDWR | O_CREAT, 0644);
        if (g_disks[i].fd < 0) {
            LOG_ERROR("cannot open target disk %d: %s (errno=%d)",
                      i, g_disks[i].path, errno);
            return -1;
        }
        LOG_INFO("opened target disk %d: %s (fd=%d)",
                 i, g_disks[i].path, g_disks[i].fd);
    }
    return 0;
}

void block_writer_close_all(void) {
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        if (g_disks[i].fd >= 0) {
            fsync(g_disks[i].fd);
            close(g_disks[i].fd);
            g_disks[i].fd = -1;
            LOG_INFO("closed target disk %d", i);
        }
    }
}

int block_writer_fsync_all(void) {
    int rc = 0;
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        if (g_disks[i].fd >= 0) {
            if (fsync(g_disks[i].fd) < 0) {
                LOG_ERROR("fsync disk %d failed: errno=%d", i, errno);
                rc = -1;
            }
        }
    }
    LOG_INFO("fsync all disks %s", rc == 0 ? "OK" : "FAILED");
    return rc;
}

int block_writer_write(int32_t devno, int64_t offset,
                       const uint8_t *data, uint32_t len) {
    if (devno < 0 || devno >= MAX_TARGET_DISKS || !g_disks[devno].registered) {
        LOG_ERROR("invalid devno %d", devno);
        return -1;
    }

    int fd = g_disks[devno].fd;
    if (fd < 0) {
        LOG_ERROR("disk %d not open", devno);
        return -1;
    }

    /* lseek to write position */
    if (lseek64(fd, offset, SEEK_SET) < 0) {
        LOG_ERROR("lseek(devno=%d, offset=%lld) failed: errno=%d",
                  devno, (long long)offset, errno);
        return -1;
    }

    /* Loop write handling short writes and signal interrupts */
    uint32_t written = 0;
    while (written < len) {
#ifdef _WIN32
        int n = _write(fd, data + written, len - written);
#else
        ssize_t n = write(fd, data + written, len - written);
#endif
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("write(devno=%d, offset=%lld, len=%u) failed: errno=%d",
                      devno, (long long)offset, len, errno);
            return -1;
        }
        written += (uint32_t)n;
    }

    /* Update stats */
    g_total_blocks++;
    g_total_bytes += len;
    g_pending_bytes += len;

    /* Periodic fsync: flush every FSYNC_BLOCK_INTERVAL blocks (~2GB) */
    g_blocks_since_fsync++;
    if (g_blocks_since_fsync >= FSYNC_BLOCK_INTERVAL) {
        block_writer_fsync_all();
        g_blocks_since_fsync = 0;
    }

    LOG_DEBUG("wrote block devno=%d offset=%lld size=%u (total: %llu blocks, %llu bytes)",
              devno, (long long)offset, len,
              (unsigned long long)g_total_blocks,
              (unsigned long long)g_total_bytes);
    return 0;
}

int block_writer_has_registered(void) {
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        if (g_disks[i].registered) return 1;
    }
    return 0;
}

uint64_t block_writer_total_blocks(void) { return g_total_blocks; }
uint64_t block_writer_total_bytes(void)  { return g_total_bytes; }

uint64_t block_writer_pending_bytes(void) {
    uint64_t pending = g_pending_bytes;
    g_pending_bytes = 0;
    return pending;
}
