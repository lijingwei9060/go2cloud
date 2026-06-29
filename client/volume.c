/*
 * volume.c — 卷枚举模块实现 (Windows)
 *
 * 步骤:
 *   1. 枚举 \\.\PhysicalDriveN (N = 0..15)
 *   2. 通过 IOCTL 读取磁盘几何信息和设备编号
 *   3. 过滤: 仅固定磁盘 (MediaType == FixedMedia)
 *   4. 排除可移动介质和 CD-ROM
 */

#include "volume.h"
#include "../include/protocol.h"
#include "log.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

int volume_enumerate(volume_list_t *list) {
    memset(list, 0, sizeof(*list));

    for (int n = 0; n < MAX_VOLUMES; n++) {
        char path[VOLUME_PATH_MAX];
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", n);

        HANDLE h = CreateFileA(
            path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (h == INVALID_HANDLE_VALUE) {
            continue;  /* 磁盘不存在或无权访问 → 跳至下一个 */
        }

        /* 检查介质类型: 仅固定磁盘 */
        DISK_GEOMETRY geom;
        DWORD bytes_returned = 0;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                             NULL, 0, &geom, sizeof(geom),
                             &bytes_returned, NULL)) {
            CloseHandle(h);
            continue;
        }

        if (geom.MediaType != FixedMedia) {
            LOG_DEBUG("volume: skipping PhysicalDrive%d (media type %d)",
                      n, geom.MediaType);
            CloseHandle(h);
            continue;
        }

        /* 获取设备编号 (用于与配置匹配) */
        STORAGE_DEVICE_NUMBER dev_num;
        memset(&dev_num, 0, sizeof(dev_num));
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            NULL, 0, &dev_num, sizeof(dev_num),
                            &bytes_returned, NULL)) {
            /* devno 使用设备编号 (可能不同于枚举 N) */
            n = (int)dev_num.DeviceNumber;  /* 覆盖循环变量 */
        }

        /* 计算总容量 */
        uint64_t total_bytes = (uint64_t)geom.Cylinders.QuadPart
                             * geom.TracksPerCylinder
                             * geom.SectorsPerTrack
                             * geom.BytesPerSector;

        volume_info_t *vol = &list->volumes[list->count];
        vol->devno         = (int32_t)n;
        vol->total_sectors = (uint64_t)geom.Cylinders.QuadPart
                            * geom.TracksPerCylinder
                            * geom.SectorsPerTrack;
        vol->total_bytes   = total_bytes;
        vol->block_count   = volume_block_count(vol, (uint32_t)BLOCK_SIZE);
        vol->writable      = 0;  /* 源端只读 */
        snprintf(vol->path, sizeof(vol->path), "%s", path);

        LOG_INFO("volume: PhysicalDrive%d — %.2f GB (%llu blocks @ 1MB)",
                 n, (double)total_bytes / (1024.0 * 1024.0 * 1024.0),
                 (unsigned long long)vol->block_count);

        CloseHandle(h);
        list->count++;

        if (list->count >= MAX_VOLUMES) break;
    }

    LOG_INFO("volume: enumerated %d fixed disks", list->count);
    return list->count > 0 ? 0 : -1;
}

#else  /* ! _WIN32 — Linux 桩实现 */

int volume_enumerate(volume_list_t *list) {
    memset(list, 0, sizeof(*list));

    /*
     * Linux 下磁盘路径由配置文件指定, 这里注册一个占位卷。
     * 实际使用时通过配置文件 (user.json) 中的 Disks 指定。
     */
    volume_info_t *vol = &list->volumes[0];
    vol->devno       = 0;
    vol->total_bytes = 0;
    vol->block_count = 0;
    vol->writable    = 0;
    snprintf(vol->path, sizeof(vol->path), "/dev/sda");
    list->count = 1;

    LOG_WARN("volume: Linux volume enumeration is stub, use config file");
    return 0;
}

#endif /* _WIN32 */

uint64_t volume_block_count(const volume_info_t *vol, uint32_t block_size) {
    if (block_size == 0) return 0;
    uint64_t count = vol->total_bytes / block_size;
    /* 余数构成最后一个不完整块 */
    if (vol->total_bytes % block_size != 0) count++;
    return count;
}
