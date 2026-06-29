/*
 * volume.h — 卷枚举模块 (Windows)
 *
 * 枚举系统中的固定磁盘卷, 获取磁盘编号、大小和路径。
 *
 * 使用 Windows API:
 *   - CreateFile(\.\PhysicalDriveN)  打开磁盘
 *   - DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY) 获取大小
 *   - DeviceIoControl(IOCTL_STORAGE_GET_DEVICE_NUMBER) 获取设备号
 *
 * 仅在 _WIN32 下编译, Linux 下为桩实现。
 */

#ifndef CLIENT_VOLUME_H
#define CLIENT_VOLUME_H

#include <stdint.h>

#define MAX_VOLUMES      16       /* 最大磁盘数 */
#define VOLUME_PATH_MAX  256      /* 磁盘路径最大长度 */

/* 一个磁盘卷 */
typedef struct {
    int32_t  devno;               /* 磁盘编号 (0, 1, 2, ...) */
    char     path[VOLUME_PATH_MAX]; /* 磁盘路径 (如 \\.\PhysicalDrive0) */
    uint64_t total_sectors;       /* 总扇区数 */
    uint64_t total_bytes;         /* 总字节数 */
    uint64_t block_count;         /* 块数 (按 1MB 计算) */
    int      writable;            /* 是否可写 (源端仅读) */
} volume_info_t;

/* 卷列表 */
typedef struct {
    volume_info_t volumes[MAX_VOLUMES];
    int           count;
} volume_list_t;

/*
 * 枚举系统中所有固定磁盘。
 * 仅在 Windows 上返回有效结果 (Linux 上返回 0)。
 *
 * 排除可移动介质和 CD-ROM。
 *
 * 返回 0 成功, -1 错误。
 */
int volume_enumerate(volume_list_t *list);

/*
 * 获取指定卷的可读块数。
 *
 * devno: 磁盘编号
 * block_size: 块大小 (1MB)
 *
 * 返回块数, 0 表示无效磁盘。
 */
uint64_t volume_block_count(const volume_info_t *vol, uint32_t block_size);

#endif /* CLIENT_VOLUME_H */
