/*
 * protocol.h — go2cloud 块级迁移协议定义
 *
 * 本文件定义客户端(Transfer)和服务端(Receiver)之间通信的完整有线协议。
 * 客户端和服务端共享此文件以确保格式一致。
 *
 * 协议栈 (自底向上):
 *   第 1 层 — TCP 分帧:    [4 字节大端长度] + [帧数据]
 *   第 2 层 — 帧同步:      "abc" 魔数 (0x61 0x62 0x63) + 载荷
 *   第 3 层 — 压缩:        Zstd 压缩 (级别 1~7)
 *   第 4 层 — 消息:        MsgPack fixmap(3){devno, offset, data}
 *
 * 服务端响应 (20 字节固定):
 *   struct { int32 type; int32 devno; int32 size; int64 offset; }
 *
 * 控制消息 (不经 MsgPack/Zstd 编码):
 *   "ctlIncremental"     — 增量模式开始
 *   "ctlEndIncremental"  — 增量模式结束
 */

#ifndef GO2CLOUD_PROTOCOL_H
#define GO2CLOUD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 第 1 层 — TCP 分帧层
 * ================================================================ */

/* TCP 帧头: 4 字节大端序长度前缀 */
#define TCP_FRAME_HEADER_SIZE  4

/* 将 32 位长度在大端序和主机序之间转换 */
#define FRAME_LEN_HOST2NET(len)  htonl((uint32_t)(len))
#define FRAME_LEN_NET2HOST(len)  ntohl((uint32_t)(len))

/* ================================================================
 * 第 2 层 — 帧同步层
 * ================================================================ */

/* 帧同步魔数: 标记一条有效帧的开始 */
#define FRAME_MAGIC        "abc"
#define FRAME_MAGIC_LEN    3
#define FRAME_MAGIC_B0     0x61  /* 'a' */
#define FRAME_MAGIC_B1     0x62  /* 'b' */
#define FRAME_MAGIC_B2     0x63  /* 'c' */

/* ================================================================
 * 第 3 层 — 压缩层
 * ================================================================ */

/* Zstd 压缩级别范围 */
#define ZSTD_COMPRESS_LEVEL_MIN  1
#define ZSTD_COMPRESS_LEVEL_MAX  7

/* ================================================================
 * 第 4 层 — MsgPack 消息层
 * ================================================================ */

/* MsgPack 类型标记 */
#define MSGPACK_FIXMAP_BASE  0x80  /* 1000xxxx: 低 4 位 = 元素数 */
#define MSGPACK_FIXMAP_3     0x93  /* fixmap(3) — 三条键值对 */
#define MSGPACK_FIXINT_POS   0x00  /* 0xxxxxxx: 正整数 */
#define MSGPACK_FIXINT_NEG   0xe0  /* 111xxxxx: 负整数 */
#define MSGPACK_BIN8         0xc4  /* 1 字节长度 */
#define MSGPACK_BIN16        0xc5  /* 2 字节长度 (大端) */
#define MSGPACK_BIN32        0xc6  /* 4 字节长度 (大端) */
#define MSGPACK_INT8         0xd0  /* 1 字节有符号整数 */
#define MSGPACK_INT16        0xd1  /* 2 字节有符号整数 */
#define MSGPACK_INT32        0xd2  /* 4 字节有符号整数 */
#define MSGPACK_INT64        0xd3  /* 8 字节有符号整数 */

/* MsgPack 消息中的固定键 */
#define MSGPACK_KEY_DEVNO    0     /* 磁盘编号 */
#define MSGPACK_KEY_OFFSET   1     /* 字节偏移 */
#define MSGPACK_KEY_DATA     2     /* 块数据 */

/* ================================================================
 * 控制消息
 *
 * 控制消息使用与块数据相同的 TCP 帧层封装 (4B BE len + payload),
 * 但 payload 是原始 UTF-8 字符串, 不经 MsgPack 编码或 Zstd 压缩。
 * 服务端通过 payload 不包含 "abc" 魔数来区分控制消息和数据消息。
 * ================================================================ */

#define CTL_INCREMENTAL         "ctlIncremental"
#define CTL_INCREMENTAL_LEN     15

#define CTL_END_INCREMENTAL     "ctlEndIncremental"
#define CTL_END_INCREMENTAL_LEN 17

/* ================================================================
 * 服务端响应
 *
 * 服务端对每条收到的消息 (控制或数据) 返回一个 20 字节固定结构体。
 * 结构体以原始字节形式发送, 不经任何编码或分帧。
 * ================================================================ */

#pragma pack(push, 1)
typedef struct {
    int32_t  type;      /* 响应类型, 见 response_type_t */
    int32_t  devno;     /* 磁盘编号 (回显) */
    int32_t  size;      /* 块大小, 用于 ACK 匹配 (回显) */
    int64_t  offset;    /* 块偏移, 用于 ACK 匹配 (回显) */
} server_response_t;
#pragma pack(pop)

#define SERVER_RESPONSE_SIZE  20  /* sizeof(server_response_t) */

/* 服务端响应类型 */
typedef enum {
    RESPONSE_ACK         = 0,   /* 块确认: 服务端已持久化该块 */
    RESPONSE_BINLOG      = 1,   /* binlog 通知: 服务端 binlog 大小告警 */
    RESPONSE_SERVER_DONE = 2,   /* 服务端操作完成 (如增量轮次 fsync 完成) */
    RESPONSE_ACK_ALT     = 4    /* 块确认变体, 同 RESPONSE_ACK */
} response_type_t;

/* ================================================================
 * 块级传输常量
 * ================================================================ */

#define BLOCK_SIZE               0x100000ULL  /* 标准块大小: 1MB = 1048576 */
#define BLOCK_SIZE_MAX           BLOCK_SIZE
#define BINLOG_SIZE_LIMIT        0x280000000ULL  /* binlog 上限: 10GB */

/* ================================================================
 * 连接管理常量
 * ================================================================ */

#define SOCKET_POOL_TARGET       7     /* 目标并发 TCP 连接数 */
#define MAX_CONNECT_RETRIES      6     /* 初始连接最大重试次数 */
#define STALE_SOCKET_TIMEOUT_SEC 720   /* 12 分钟无活动 → 关闭 */
#define RETRANSMIT_MIN_INTERVAL_SEC 121 /* 最小重传间隔 */

/* ================================================================
 * 背压控制常量
 * ================================================================ */

#define BACKPRESSURE_QUEUE_DEPTH  40               /* 队列深度阈值 */
#define BACKPRESSURE_MEMORY_MB    45               /* 内存使用阈值 */
#define BACKPRESSURE_SLEEP_MS     50               /* 背压等待时间 */
#define NO_SOCKET_SLEEP_MS        1000             /* 无可用 socket 等待 */

/* ================================================================
 * 定时器间隔 (毫秒)
 * ================================================================ */

#define RETRANSMIT_TIMER_MS      18000  /* 重传检查: 18 秒 */
#define RECONNECT_TIMER_MS       20000  /* 重连检查: 20 秒 */
#define RECONNECT_TIMER_REPEAT_MS 18000 /* 重连重复: 18 秒 */
#define ACTION_TIMER_MS          10000  /* 定期动作: 10 秒 */

/* 增量同步收敛控制 */
#define MAX_INCREMENTAL_ROUNDS   60     /* 最大增量轮次 (30 min @ 18s), 超时强制结束 */
#define CONVERGENCE_ZERO_ROUNDS  3      /* 连续 N 轮无变化块 → 判定收敛, 强制 allDone */

/* ================================================================
 * 消息缓冲区大小
 * ================================================================ */

#define MSGPACK_BUF_INITIAL_CAP  0x2000   /* MsgPack 缓冲区初始容量: 8KB */
#define SQL_BUF_SIZE             512       /* SQL 语句缓冲区 */
#define LOG_BUF_SIZE             4096      /* 日志缓冲区 */

/* ================================================================
 * 客户端消息结构 (内存表示, 非有线格式)
 * ================================================================ */

typedef struct {
    int32_t  devno;        /* 磁盘编号 */
    int64_t  offset;       /* 块在磁盘上的字节偏移 */
    uint8_t *data;         /* 块数据指针 (1MB) */
    uint32_t data_len;     /* 数据实际长度 (最后一块可能 < 1MB) */
} block_message_t;

/* ================================================================
 * 哈希算法常量
 *
 * hash_block 是基于 xxHash64 结构的自定义哈希。
 * 使用非标准常数以避免碰撞攻击。
 * ================================================================ */

#define HASH_MUL         0xc2b4d1b3d54d51c2ULL
#define HASH_ROT         0x9e3779b586c04815ULL
#define HASH_INIT_SHORT  0x27d4eb2f165667c5ULL
#define HASH_INIT_A      0x60ea27eeadc0b5d6ULL
#define HASH_INIT_B      0x61c8864e7a143579ULL
#define HASH_INIT_C      0xc2b2ae3d27d4eb4fULL
#define HASH_XOR_CONST   0x85ebca77c2b2ae63ULL

/* ================================================================
 * 辅助宏
 * ================================================================ */

/* 最小值/最大值 */
#define MIN(a, b)  ((a) < (b) ? (a) : (b))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

#ifdef __cplusplus
}
#endif

#endif /* GO2CLOUD_PROTOCOL_H */
