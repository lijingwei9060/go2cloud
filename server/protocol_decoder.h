/*
 * protocol_decoder.h — 协议解码模块
 *
 * 处理从 TCP 字节流到业务消息的完整解码流程:
 *   1. TCP 分帧层: 提取 4B 大端长度 → 缓冲完整帧
 *   2. 帧同步层: 验证 "abc" 魔数 (数据帧) 或跳过 (控制帧)
 *   3. 压缩层: Zstd 解压
 *   4. 消息层: MsgPack 解码 → {devno, offset, data}
 *
 * 解码结果通过回调函数返回给上层。
 */

#ifndef SERVER_PROTOCOL_DECODER_H
#define SERVER_PROTOCOL_DECODER_H

#include <stdint.h>
#include <stddef.h>

/* 前向声明 (session.h 中定义) */
typedef struct session session_t;

/* 解码结果类型 */
typedef enum {
    DECODE_DATA_BLOCK,       /* 数据块: devno, offset, data, data_len 有效 */
    DECODE_CTL_INCREMENTAL,  /* 控制: 增量模式开始 */
    DECODE_CTL_END_INCREMENTAL, /* 控制: 增量模式结束 */
    DECODE_ERROR             /* 解码失败 */
} decode_result_t;

/* 解码结果 */
typedef struct {
    decode_result_t type;
    int32_t  devno;          /* 磁盘编号 (仅 DATA_BLOCK) */
    int64_t  offset;         /* 字节偏移 (仅 DATA_BLOCK) */
    uint8_t  data[1024 * 1024 + 4096]; /* 块数据缓冲 (1MB + 头部余量) */
    uint32_t data_len;       /* 实际数据长度 */
} decoded_msg_t;

/*
 * 向解码器喂入新收到的原始 TCP 数据。
 *
 * buf:    新收到的数据
 * len:    数据长度
 * out:    输出解码结果 (仅当返回 1 时有效)
 *
 * 返回:  0 = 需要更多数据 (out 无效)
 *        1 = 已解码一条完整消息 (out 有效)
 *       -1 = 解码错误 (连接应关闭)
 *
 * 解码器在 session 内部维护状态机, 处理 TCP 粘包/拆包。
 */
int protocol_decode(session_t *session, const uint8_t *buf, size_t len,
                    decoded_msg_t *out);

/*
 * 错误消息转字符串
 */
const char *decode_error_string(int err);

#endif /* SERVER_PROTOCOL_DECODER_H */
