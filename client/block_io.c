/*
 * block_io.c — 块级磁盘读取模块实现
 *
 * Windows: 使用 CreateFile + ReadFile, 扇区对齐。
 *          块读取必须对齐到扇区边界 (512 字节)。
 *          使用系统缓存读取以获得更好性能。
 *
 * Linux:   使用 open + pread64, 无对齐要求。
 */

#include "block_io.h"
#include "../include/protocol.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

struct block_reader {
    HANDLE   handle;
    uint64_t total_bytes;
};

block_reader_t *block_reader_open(const char *path) {
    HANDLE h = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,  /* 顺序读取优化 */
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR("block_reader_open: CreateFile(%s) failed: %lu",
                  path, GetLastError());
        return NULL;
    }

    /* 获取磁盘大小 */
    GET_LENGTH_INFORMATION len_info;
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO,
                         NULL, 0, &len_info, sizeof(len_info),
                         &bytes_returned, NULL)) {
        LOG_ERROR("block_reader_open: IOCTL_DISK_GET_LENGTH_INFO failed: %lu",
                  GetLastError());
        CloseHandle(h);
        return NULL;
    }

    block_reader_t *r = malloc(sizeof(*r));
    if (!r) {
        CloseHandle(h);
        return NULL;
    }
    r->handle      = h;
    r->total_bytes = (uint64_t)len_info.Length.QuadPart;

    LOG_INFO("block_reader: opened %s (%.2f GB)", path,
             (double)r->total_bytes / (1024.0 * 1024.0 * 1024.0));
    return r;
}

void block_reader_close(block_reader_t *r) {
    if (r) {
        CloseHandle(r->handle);
        free(r);
    }
}

uint64_t block_reader_size(block_reader_t *r) {
    return r->total_bytes;
}

uint64_t block_reader_block_count(block_reader_t *r, uint32_t block_size) {
    if (block_size == 0) return 0;
    uint64_t count = r->total_bytes / block_size;
    if (r->total_bytes % block_size != 0) count++;
    return count;
}

int block_reader_read(block_reader_t *r, uint64_t offset,
                      uint8_t *buf, uint32_t buf_size,
                      uint32_t *bytes_read) {
    /* 扇区对齐验证 (块大小 1MB 自然对齐, 检查以防万一) */
    if (offset % 512 != 0) {
        LOG_WARN("block_reader_read: offset %llu not sector-aligned",
                 (unsigned long long)offset);
    }

    /* 设置文件指针 (Windows 没有 pread, 使用 SetFilePointerEx + ReadFile) */
    LARGE_INTEGER li_offset;
    li_offset.QuadPart = (LONGLONG)offset;

    if (!SetFilePointerEx(r->handle, li_offset, NULL, FILE_BEGIN)) {
        LOG_ERROR("block_reader_read: SetFilePointerEx(%llu) failed: %lu",
                  (unsigned long long)offset, GetLastError());
        return -1;
    }

    /* 限制读取大小: 不超过文件剩余部分 */
    DWORD to_read = (DWORD)MIN((uint64_t)buf_size, r->total_bytes - offset);

    DWORD n_read = 0;
    if (!ReadFile(r->handle, buf, to_read, &n_read, NULL)) {
        LOG_ERROR("block_reader_read: ReadFile(%llu, %u) failed: %lu",
                  (unsigned long long)offset, to_read, GetLastError());
        return -1;
    }

    *bytes_read = (uint32_t)n_read;
    return 0;
}

#else  /* Linux */

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

struct block_reader {
    int      fd;
    uint64_t total_bytes;
};

block_reader_t *block_reader_open(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        /* O_DIRECT 可能失败 (权限不足), 回退到普通打开 */
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOG_ERROR("block_reader_open: open(%s) failed", path);
            return NULL;
        }
    }

    /* 获取磁盘大小 */
    uint64_t size = (uint64_t)lseek64(fd, 0, SEEK_END);
    if (size == (uint64_t)-1) {
        LOG_ERROR("block_reader_open: lseek64(%s) failed", path);
        close(fd);
        return NULL;
    }
    lseek64(fd, 0, SEEK_SET);

    block_reader_t *r = malloc(sizeof(*r));
    if (!r) {
        close(fd);
        return NULL;
    }
    r->fd          = fd;
    r->total_bytes = size;

    LOG_INFO("block_reader: opened %s (%.2f GB)", path,
             (double)size / (1024.0 * 1024.0 * 1024.0));
    return r;
}

void block_reader_close(block_reader_t *r) {
    if (r) {
        close(r->fd);
        free(r);
    }
}

uint64_t block_reader_size(block_reader_t *r) {
    return r->total_bytes;
}

uint64_t block_reader_block_count(block_reader_t *r, uint32_t block_size) {
    if (block_size == 0) return 0;
    uint64_t count = r->total_bytes / block_size;
    if (r->total_bytes % block_size != 0) count++;
    return count;
}

int block_reader_read(block_reader_t *r, uint64_t offset,
                      uint8_t *buf, uint32_t buf_size,
                      uint32_t *bytes_read) {
    uint32_t to_read = (uint32_t)MIN((uint64_t)buf_size, r->total_bytes - offset);

    ssize_t n = pread64(r->fd, buf, to_read, (off64_t)offset);
    if (n < 0) {
        LOG_ERROR("block_reader_read: pread64(%llu, %u) failed",
                  (unsigned long long)offset, to_read);
        return -1;
    }

    *bytes_read = (uint32_t)n;
    return 0;
}

#endif /* _WIN32 */
