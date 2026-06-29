/*
 * hash.h — 块哈希模块
 *
 * 实现自定义 64-bit 哈希函数, 用于块内容去重检测。
 * 相同偏移 + 相同哈希 = 内容未变 → 跳过传输。
 *
 * 算法基于 xxHash64 结构:
 *   - 32 字节块处理 (4 × uint64 并行)
 *   - 31 位右旋 + 乘法混合
 *   - 自定义常数 (非标准, 防碰撞攻击)
 */

#ifndef CLIENT_HASH_H
#define CLIENT_HASH_H

#include <stdint.h>
#include <stddef.h>

/*
 * 计算块的 64-bit 哈希值。
 *
 * data: 块数据指针
 * len:  数据长度 (通常 1MB)
 * seed: 外部种子 (可用于增量轮次区分)
 *
 * 返回: 64-bit 哈希值
 *
 * 短数据 (< 32 字节): len + 常数 + seed
 * 长数据 (≥ 32 字节): 完整 4 状态迭代混合
 */
uint64_t hash_block(const void *data, size_t len, uint64_t seed);

#endif /* CLIENT_HASH_H */
