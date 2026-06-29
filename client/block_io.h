/*
 * block_io.h — 块级磁盘读取模块
 *
 * 从源磁盘按固定大小 (1MB) 读取块数据。
 * 支持跨平台:
 *   - Windows: CreateFile + ReadFile + 扇区对齐
 *   - Linux:   open + pread64
 *
 * 每次读取一个块, 调用者负责分配缓冲区 (≥ BLOCK_SIZE)。
 */

#ifndef CLIENT_BLOCK_IO_H
#define CLIENT_BLOCK_IO_H

#include <stdint.h>
#include <stddef.h>

/* 不透明磁盘句柄 */
typedef struct block_reader block_reader_t;

/*
 * 打开源磁盘 (只读)。
 *
 * path: 磁盘路径 (如 \\.\PhysicalDrive0 或 /dev/sda)
 *
 * 返回不透明句柄, 失败返回 NULL。
 */
block_reader_t *block_reader_open(const char *path);

/* 关闭磁盘 */
void block_reader_close(block_reader_t *r);

/*
 * 获取磁盘总大小 (字节)。
 */
uint64_t block_reader_size(block_reader_t *r);

/*
 * 计算磁盘按块大小划分的总块数。
 */
uint64_t block_reader_block_count(block_reader_t *r, uint32_t block_size);

/*
 * 读取一个块。
 *
 * r:          磁盘句柄
 * offset:     字节偏移 (必须扇区对齐)
 * buf:        输出缓冲区
 * buf_size:   缓冲区大小 (应 ≥ block_size)
 * bytes_read: [out] 实际读取的字节数
 *
 * 返回: 0 成功, -1 读取错误
 *
 * 注意: 最后一块可能小于 block_size, 由 bytes_read 指示实际长度。
 */
int block_reader_read(block_reader_t *r, uint64_t offset,
                      uint8_t *buf, uint32_t buf_size,
                      uint32_t *bytes_read);

#endif /* CLIENT_BLOCK_IO_H */
