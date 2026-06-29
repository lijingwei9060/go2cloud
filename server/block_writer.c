/*
 * block_writer.c — 目标磁盘块写入模块实现
 *
 * 实现带重试的 lseek + write 循环, 确保完整写入每个块。
 */

#include "block_writer.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#define lseek64  _lseeki64
#define write    _write
#define fsync    _commit
#define open     _open
#define O_RDWR   _O_RDWR
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#else
#define lseek64  lseek64
#endif

/* 目标磁盘描述 */
typedef struct {
    int   fd;           /* 打开的文件描述符, -1 表示未打开 */
    char  path[512];    /* 文件路径 */
    int   registered;   /* 是否已注册 */
} target_disk_t;

static target_disk_t g_disks[MAX_TARGET_DISKS];
static uint64_t      g_total_blocks = 0;
static uint64_t      g_total_bytes  = 0;

int block_writer_init(void) {
    memset(g_disks, 0, sizeof(g_disks));
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        g_disks[i].fd = -1;
    }
    g_total_blocks = 0;
    g_total_bytes  = 0;
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
    LOG_INFO("registered disk %d → %s", devno, filepath);
    return 0;
}

int block_writer_open_all(void) {
    for (int i = 0; i < MAX_TARGET_DISKS; i++) {
        if (!g_disks[i].registered) continue;

#ifdef _WIN32
        g_disks[i].fd = open(g_disks[i].path, O_RDWR | O_CREAT, 0644);
#else
        g_disks[i].fd = open(g_disks[i].path, O_RDWR | O_CREAT, 0644);
#endif
        if (g_disks[i].fd < 0) {
            LOG_ERROR("cannot open target disk %d: %s (%s)",
                      i, g_disks[i].path, strerror(errno));
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
                LOG_ERROR("fsync disk %d failed: %s", i, strerror(errno));
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

    /* lseek 到写入位置 */
    if (lseek64(fd, offset, SEEK_SET) < 0) {
        LOG_ERROR("lseek(devno=%d, offset=%lld) failed: %s",
                  devno, (long long)offset, strerror(errno));
        return -1;
    }

    /* 循环写入, 处理短写和信号中断 */
    uint32_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;  /* 被信号中断, 重试 */
            LOG_ERROR("write(devno=%d, offset=%lld, len=%u) failed: %s",
                      devno, (long long)offset, len, strerror(errno));
            return -1;
        }
        written += n;
    }

    /* 更新统计 */
    g_total_blocks++;
    g_total_bytes += len;

    LOG_DEBUG("wrote block devno=%d offset=%lld size=%u (total: %llu blocks, %llu bytes)",
              devno, (long long)offset, len,
              (unsigned long long)g_total_blocks,
              (unsigned long long)g_total_bytes);
    return 0;
}

uint64_t block_writer_total_blocks(void) { return g_total_blocks; }
uint64_t block_writer_total_bytes(void)  { return g_total_bytes; }
