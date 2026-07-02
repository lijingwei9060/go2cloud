/*
 * dcbt_ioctl.h — shared IOCTL definitions for go2cloud driver + client
 *
 * Included by both kernel-mode driver code and user-mode client code.
 * Uses #ifdef _KERNEL_MODE to select appropriate system headers.
 */

#ifndef DCBT_IOCTL_H
#define DCBT_IOCTL_H

#ifdef _KERNEL_MODE
#include <ntifs.h>
#include <wdf.h>
#else
#include <windows.h>
#endif

#define GO2CLOUD_FLT_DEVICE_TYPE  FILE_DEVICE_UNKNOWN

/* IOCTL codes */
#define IOCTL_GO2CLOUD_GET_BITMAP \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_GO2CLOUD_CLEAR_BITMAP \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define IOCTL_GO2CLOUD_GET_STATS \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_GO2CLOUD_GET_VERSION \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE \
    CTL_CODE(GO2CLOUD_FLT_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Driver constants */
#define DCBT_BLOCK_SIZE       (1024 * 1024)  /* 1 MB granularity */
#define DCBT_VERSION_MAJOR    2
#define DCBT_VERSION_MINOR    0
#define DCBT_VERSION_PATCH    0
#define DCBT_MAX_DISKS        64

/* FIELD_OFFSET is a kernel-mode macro; provide fallback for user mode */
#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)(LONG_PTR)&(((type *)0)->field))
#endif

/* Control device name — user mode opens \\.\go2cloud_flt */
#define DCBT_DEVICE_NAME      L"\\Device\\go2cloud_flt"
#define DCBT_SYMLINK_NAME     L"\\DosDevices\\go2cloud_flt"

#pragma pack(push, 8)

/* IOCTL_GO2CLOUD_GET_BITMAP: input = DiskNumber, output = GO2CLOUD_BITMAP */
typedef struct _GO2CLOUD_BITMAP {
    unsigned long  DiskNumber;
    unsigned long long TotalBytes;
    unsigned long  BlockSize;
    unsigned long  TotalBlocks;
    unsigned long  DirtyBlocks;
    unsigned long  BitmapSize;       /* (TotalBlocks + 7) / 8 */
    unsigned char  Bitmap[1];        /* variable-length payload */
} GO2CLOUD_BITMAP;
typedef GO2CLOUD_BITMAP *PGO2CLOUD_BITMAP;

/* IOCTL_GO2CLOUD_QUERY_BITMAP_SIZE: input+output */
typedef struct _GO2CLOUD_BITMAP_SIZE {
    unsigned long  DiskNumber;
    unsigned long  TotalBlocks;
    unsigned long  BitmapSize;       /* bytes user must allocate */
} GO2CLOUD_BITMAP_SIZE;
typedef GO2CLOUD_BITMAP_SIZE *PGO2CLOUD_BITMAP_SIZE;

/* IOCTL_GO2CLOUD_CLEAR_BITMAP: input = DiskNumber */
/* IOCTL_GO2CLOUD_GET_STATS: input = DiskNumber, output = GO2CLOUD_STATS */
typedef struct _GO2CLOUD_STATS {
    unsigned long  DiskNumber;
    unsigned long long TotalWrites;
    unsigned long long TotalBytesWritten;
    unsigned long long FirstWriteTick;  /* KeQueryTickCount() units */
    unsigned long long LastWriteTick;
} GO2CLOUD_STATS;
typedef GO2CLOUD_STATS *PGO2CLOUD_STATS;

/* IOCTL_GO2CLOUD_GET_VERSION: output only */
typedef struct _GO2CLOUD_VERSION {
    unsigned short Major;
    unsigned short Minor;
    unsigned short Patch;
} GO2CLOUD_VERSION;
typedef GO2CLOUD_VERSION *PGO2CLOUD_VERSION;

#pragma pack(pop)

#endif /* DCBT_IOCTL_H */
