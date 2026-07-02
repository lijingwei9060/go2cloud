/*
 * volume.h — 卷枚举模块 (Windows)
 *
 * 枚举系统中的固定磁盘及其分区布局。
 * 每个分区映射到一个源块设备, 支持 VSS 快照路径。
 *
 * 使用 Windows API:
 *   - CreateFile(\\.\PhysicalDriveN)         打开磁盘
 *   - DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY)       获取磁盘大小
 *   - DeviceIoControl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX)      获取分区布局
 *   - DeviceIoControl(IOCTL_STORAGE_GET_DEVICE_NUMBER)     获取设备号
 *   - DeviceIoControl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) 获取卷磁盘映射
 *
 * 仅在 _WIN32 下编译, Linux 下为桩实现。
 */

#ifndef CLIENT_VOLUME_H
#define CLIENT_VOLUME_H

#include <stdint.h>

#define MAX_VOLUMES      64       /* 最大分区数 (跨所有磁盘) */
#define VOLUME_PATH_MAX  256      /* 磁盘路径最大长度 */
#define VSS_PATH_MAX      512     /* VSS 快照路径最大长度 */

/* 一个分区 (源迁移单元) */
typedef struct {
    int32_t  devno;                    /* 所在物理磁盘编号 */
    char     disk_path[VOLUME_PATH_MAX]; /* PhysicalDrive 路径 (回退用) */
    char     vss_path[VSS_PATH_MAX];   /* VSS 快照路径 (如 \\?\GLOBALROOT\...\ShadowCopyN) */
    char     name[16];                 /* 卷名 (如 "C:") */
    uint64_t partition_offset;         /* 分区在物理磁盘上的起始偏移 (字节) */
    uint64_t total_bytes;              /* 分区大小 (字节) */
    uint64_t total_sectors;            /* 分区大小 (扇区数) */
    uint64_t block_count;              /* 可传输块数 (按 1MB 计算) */
    int      writable;                 /* 是否可写 (源端仅读) */
    int      has_vss;                  /* 1 = 已创建 VSS 快照 */
    int      part_index;               /* 分区在磁盘上的序号 (从 1 开始) */
} volume_info_t;

/* 卷列表 */
typedef struct {
    volume_info_t volumes[MAX_VOLUMES];
    int           count;
} volume_list_t;

/*
 * 枚举系统中所有固定磁盘上的分区。
 * 仅在 Windows 上返回有效结果 (Linux 上返回 0)。
 *
 * 排除可移动介质和 CD-ROM。
 * 每个分区作为一个独立条目, 包含其在物理磁盘上的偏移和大小。
 *
 * 返回 0 成功, -1 错误。
 */
int volume_enumerate(volume_list_t *list);

/*
 * 获取指定卷的可读块数。
 *
 * vol:        卷信息
 * block_size: 块大小 (1MB)
 *
 * 返回块数, 0 表示无效卷。
 */
uint64_t volume_block_count(const volume_info_t *vol, uint32_t block_size);

#endif /* CLIENT_VOLUME_H */
