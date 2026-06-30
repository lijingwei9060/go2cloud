/*
 * volume.c — 卷枚举模块实现 (Windows)
 *
 * 枚举系统中所有固定磁盘上的分区。
 * 每个分区作为一个独立的 volume_info_t 条目。
 *
 * 步骤:
 *   1. 枚举 \\.\PhysicalDriveN (N = 0..15)
 *   2. 过滤: 仅固定磁盘 (MediaType == FixedMedia)
 *   3. 通过 IOCTL_DISK_GET_DRIVE_LAYOUT_EX 获取分区布局
 *   4. 通过 FindFirstVolumeW + GetVolumePathNamesForVolumeNameW
 *      + IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS 映射盘符
 *   5. 填充 volume_info_t (磁盘路径 / VSS 路径 / 分区偏移 / 盘符)
 *
 * 使用 Windows API:
 *   - CreateFile(\\.\PhysicalDriveN)
 *   - DeviceIoControl(IOCTL_DISK_GET_DRIVE_GEOMETRY)
 *   - DeviceIoControl(IOCTL_STORAGE_GET_DEVICE_NUMBER)
 *   - DeviceIoControl(IOCTL_DISK_GET_DRIVE_LAYOUT_EX)
 *   - DeviceIoControl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)
 *   - FindFirstVolumeW / FindNextVolumeW
 *   - GetVolumePathNamesForVolumeNameW
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

    /*
     * 第 1 步: 枚举所有物理磁盘上的分区。
     */
    for (int n = 0; n < 16; n++) {
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
            continue;  /* 磁盘不存在或无权访问 */
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

        /* 获取设备编号 */
        int32_t devno = (int32_t)n;
        STORAGE_DEVICE_NUMBER dev_num;
        memset(&dev_num, 0, sizeof(dev_num));
        if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            NULL, 0, &dev_num, sizeof(dev_num),
                            &bytes_returned, NULL)) {
            devno = (int32_t)dev_num.DeviceNumber;
        }

        /* 获取分区布局 — 分配足够大的缓冲区 */
        size_t layout_buf_size = 65536;
        void *layout_buf = calloc(1, layout_buf_size);
        if (!layout_buf) {
            CloseHandle(h);
            continue;
        }

        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                             NULL, 0, layout_buf, (DWORD)layout_buf_size,
                             &bytes_returned, NULL)) {
            LOG_DEBUG("volume: IOCTL_DISK_GET_DRIVE_LAYOUT_EX failed for PhysicalDrive%d: %lu",
                      n, GetLastError());
            free(layout_buf);
            CloseHandle(h);
            continue;
        }

        DRIVE_LAYOUT_INFORMATION_EX *layout =
            (DRIVE_LAYOUT_INFORMATION_EX *)layout_buf;

        for (DWORD p = 0; p < layout->PartitionCount; p++) {
            PARTITION_INFORMATION_EX *part = &layout->PartitionEntry[p];

            /* 跳过分区长度为 0 的条目 */
            if (part->PartitionLength.QuadPart == 0) continue;

            /* 跳过 MSR 分区 (GPT 保留分区, 无文件系统) */
            if (part->PartitionStyle == PARTITION_STYLE_GPT
                && part->Gpt.PartitionType.Data1 == 0xE3C9E316
                && part->Gpt.PartitionType.Data2 == 0x0B5C
                && part->Gpt.PartitionType.Data3 == 0x4DB8
                && part->Gpt.PartitionType.Data4[0] == 0x81
                && part->Gpt.PartitionType.Data4[1] == 0x7D
                && part->Gpt.PartitionType.Data4[2] == 0xF9
                && part->Gpt.PartitionType.Data4[3] == 0x2D
                && part->Gpt.PartitionType.Data4[4] == 0xF0
                && part->Gpt.PartitionType.Data4[5] == 0x02
                && part->Gpt.PartitionType.Data4[6] == 0x15
                && part->Gpt.PartitionType.Data4[7] == 0xAE) {
                LOG_DEBUG("volume: skipping MSR partition on disk %d", n);
                continue;
            }

            volume_info_t *vol = &list->volumes[list->count];
            memset(vol, 0, sizeof(*vol));
            vol->devno           = devno;
            snprintf(vol->disk_path, sizeof(vol->disk_path), "%s", path);
            vol->partition_offset = part->StartingOffset.QuadPart;
            vol->total_bytes      = part->PartitionLength.QuadPart;
            vol->total_sectors    = part->PartitionLength.QuadPart
                                   / geom.BytesPerSector;
            vol->block_count      = volume_block_count(vol, (uint32_t)BLOCK_SIZE);
            vol->writable         = 0;
            vol->has_vss          = 0;
            vol->vss_path[0]      = '\0';
            snprintf(vol->name, sizeof(vol->name), "Disk%dPart%lu",
                     (int)devno, (unsigned long)(p + 1));

            LOG_INFO("volume: %s offset=%llu size=%.2f GB (%llu blocks)",
                     vol->name,
                     (unsigned long long)vol->partition_offset,
                     (double)vol->total_bytes / (1024.0 * 1024.0 * 1024.0),
                     (unsigned long long)vol->block_count);

            list->count++;
            if (list->count >= MAX_VOLUMES) break;
        }

        free(layout_buf);
        CloseHandle(h);

        if (list->count >= MAX_VOLUMES) break;
    }

    /*
     * 第 2 步: 枚举卷 GUID 路径, 获取盘符和磁盘偏移,
     *         与已知分区匹配, 填充 name 字段。
     */
    WCHAR vol_name_w[64];
    HANDLE vol_find = FindFirstVolumeW(vol_name_w, 64);
    if (vol_find != INVALID_HANDLE_VALUE) {
        do {
            /* 获取盘符 (如 "C:\") */
            WCHAR drive_paths[256];
            DWORD drive_paths_len = 0;
            if (GetVolumePathNamesForVolumeNameW(vol_name_w, drive_paths,
                                                  256, &drive_paths_len)) {
                if (drive_paths[0] != L'\0') {
                    /* 转换盘符为窄字符 (取前 2 个字符, 如 "C:" ) */
                    char drive_letter[16];
                    WideCharToMultiByte(CP_ACP, 0, drive_paths, 2,
                                        drive_letter, sizeof(drive_letter),
                                        NULL, NULL);
                    drive_letter[2] = '\0';

                    /* 去掉 vol_name_w 末尾的反斜杠 (CreateFile 需要) */
                    size_t vn_len = wcslen(vol_name_w);
                    if (vn_len > 0 && vol_name_w[vn_len - 1] == L'\\') {
                        vol_name_w[vn_len - 1] = L'\0';
                    }

                    /* 打开卷获取磁盘偏移 */
                    HANDLE vh = CreateFileW(
                        vol_name_w,
                        0,  /* 无需读写权限即可查询 */
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL,
                        OPEN_EXISTING,
                        0,
                        NULL
                    );

                    if (vh != INVALID_HANDLE_VALUE) {
                        /* 为磁盘扩展分配空间 (最多 8 个扩展) */
                        size_t ext_buf_size = sizeof(VOLUME_DISK_EXTENTS)
                                             + 8 * sizeof(DISK_EXTENT);
                        VOLUME_DISK_EXTENTS *ext =
                            (VOLUME_DISK_EXTENTS *)calloc(1, ext_buf_size);
                        if (ext) {
                            DWORD ext_bytes = 0;
                            if (DeviceIoControl(vh,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    NULL, 0, ext, (DWORD)ext_buf_size,
                                    &ext_bytes, NULL)) {
                                /*
                                 * 用第一个盘区匹配已知分区。
                                 * 多盘区的卷 (跨区卷) 仅匹配主盘区。
                                 */
                                for (DWORD e = 0; e < ext->NumberOfDiskExtents; e++) {
                                    DISK_EXTENT *de = &ext->Extents[e];
                                    for (int vi = 0; vi < list->count; vi++) {
                                        volume_info_t *v = &list->volumes[vi];
                                        if ((int32_t)de->DiskNumber == v->devno &&
                                            (uint64_t)de->StartingOffset.QuadPart
                                                == v->partition_offset) {
                                            snprintf(v->name, sizeof(v->name),
                                                     "%s", drive_letter);
                                            LOG_DEBUG("volume: matched %s → %s (devno=%d offset=%llu)",
                                                      drive_letter, v->disk_path,
                                                      (int)v->devno,
                                                      (unsigned long long)v->partition_offset);
                                            break;
                                        }
                                    }
                                    /* 多盘区卷: 第一个匹配后即停止 */
                                    if (e == 0 && ext->NumberOfDiskExtents > 1) break;
                                }
                            }
                            free(ext);
                        }
                        CloseHandle(vh);
                    }
                }
            }
        } while (FindNextVolumeW(vol_find, vol_name_w, 64));
        FindVolumeClose(vol_find);
    }

    LOG_INFO("volume: enumerated %d partitions on fixed disks", list->count);
    return list->count > 0 ? 0 : -1;
}

#else  /* ! _WIN32 — Linux 桩实现 */

int volume_enumerate(volume_list_t *list) {
    memset(list, 0, sizeof(*list));

    volume_info_t *vol = &list->volumes[0];
    vol->devno       = 0;
    vol->total_bytes = 0;
    vol->block_count = 0;
    vol->writable    = 0;
    snprintf(vol->disk_path, sizeof(vol->disk_path), "/dev/sda");
    vol->partition_offset = 0;
    vol->vss_path[0] = '\0';
    snprintf(vol->name, sizeof(vol->name), "sda");
    list->count = 1;

    LOG_WARN("volume: Linux volume enumeration is stub, use config file");
    return 0;
}

#endif /* _WIN32 */

uint64_t volume_block_count(const volume_info_t *vol, uint32_t block_size) {
    if (block_size == 0) return 0;
    uint64_t count = vol->total_bytes / block_size;
    if (vol->total_bytes % block_size != 0) count++;
    return count;
}
