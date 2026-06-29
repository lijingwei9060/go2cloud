/*
 * wire.h — TCP 有线协议模块
 *
 * 实现客户端 → 服务端的有线格式发送:
 *   第 1 层: 4 字节大端帧长度 + 帧数据
 *   第 2 层: "abc" 魔数 (3 字节) + 载荷
 *   第 3 层: Zstd 压缩 (载荷部分)
 *   第 4 层: MsgPack fixmap(3) 消息
 *
 * wire_send() 接收已编码的 MsgPack 缓冲区, 自动完成 Zstd 压缩、
 * "abc" 包装和 TCP 帧化, 通过非阻塞 socket 发送。
 */

#ifndef CLIENT_WIRE_H
#define CLIENT_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET socket_t;
#else
typedef int socket_t;
#endif

/* Zstd 压缩边界, 声明所需的最小目标缓冲区大小 */
#define ZSTD_COMPRESS_BOUND(src_size)  ((src_size) + (src_size) / 8 + 64)

/*
 * 发送一条 MsgPack 编码的块消息。
 *
 * 处理流程:
 *   msgpack_buf → Zstd 压缩 → 添加 "abc" 前缀 → 添加 4B BE 长度前缀 → send()
 *
 * 参数:
 *   fd          — 目标 TCP socket
 *   msg_data    — MsgPack 编码数据
 *   msg_len     — MsgPack 编码长度
 *   zstd_level  — 压缩级别 (1~7)
 *   scratch_buf — 临时缓冲区 (至少 ZSTD_COMPRESS_BOUND(msg_len) + 7)
 *   scratch_cap — scratch_buf 容量
 *
 * 返回: 0 成功, -1 失败
 */
int wire_send(socket_t fd,
              const uint8_t *msg_data, size_t msg_len,
              int zstd_level,
              uint8_t *scratch_buf, size_t scratch_cap);

/*
 * 发送控制消息 (不经 MsgPack/Zstd)。
 *
 * 控制消息格式: 4B BE len + 原始字符串, 不含 "abc" 魔数。
 *
 * 参数:
 *   fd  — 目标 TCP socket
 *   str — 控制字符串 (如 "ctlIncremental")
 *   len — 字符串长度
 *
 * 返回: 0 成功, -1 失败
 */
int wire_send_control(socket_t fd, const char *str, size_t len);

#endif /* CLIENT_WIRE_H */
