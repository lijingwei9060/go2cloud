/*
 * block_writer.h — 目标磁盘块写入模块
 *
 * 负责将解码后的块数据写入目标磁盘的指定偏移位置。
 * 支持多个磁盘 (按 devno 索引), 每个 devno 映射到一个目标文件/设备。
 */

#ifndef SERVER_BLOCK_WRITER_H
#define SERVER_BLOCK_WRITER_H

#include <stdint.h>
#include <stddef.h>

#define MAX_TARGET_DISKS  16   /* 最大目标磁盘数 */

/* 初始化块写入器 */
int block_writer_init(void);

/* 注册一个目标磁盘: devno → filepath */
int block_writer_register(int devno, const char *filepath);

/* 打开所有已注册的目标磁盘 */
int block_writer_open_all(void);

/* 关闭所有目标磁盘 (含 fsync) */
void block_writer_close_all(void);

/* 对所有目标磁盘执行 fsync */
int block_writer_fsync_all(void);

/*
 * 写一个块到目标磁盘。
 * devno: 磁盘编号
 * offset: 在磁盘上的字节偏移
 * data: 数据指针
 * len: 数据长度
 * 返回: 0 成功, -1 失败
 */
int block_writer_write(int32_t devno, int64_t offset,
                       const uint8_t *data, uint32_t len);

/* 检查是否有磁盘已注册 */
int block_writer_has_registered(void);

/* 获取已写入块的总数 */
uint64_t block_writer_total_blocks(void);

/* 获取已写入字节总数 */
uint64_t block_writer_total_bytes(void);

#endif /* SERVER_BLOCK_WRITER_H */
