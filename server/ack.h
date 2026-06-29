/*
 * ack.h — ACK 响应生成模块
 *
 * 服务端对每条收到的消息返回一个 20 字节的 server_response_t 结构体。
 * 响应不经任何编码, 直接通过 TCP 发送原始字节。
 */

#ifndef SERVER_ACK_H
#define SERVER_ACK_H

#include <stdint.h>

/*
 * 发送块 ACK 响应 (type=0)。
 * 回显 devno/size/offset 供客户端匹配未确认块。
 */
int ack_send_block(int fd, int32_t devno, int32_t size, int64_t offset);

/*
 * 发送服务端完成响应 (type=2)。
 * 在所有目标磁盘 fsync 完成后调用, 通知客户端增量轮次结束。
 */
int ack_send_done(int fd);

/*
 * 发送 binlog 告警响应 (type=1)。
 * 当 binlog 大小超过 10GB 上限时调用。
 */
int ack_send_binlog(int fd, int32_t devno, int32_t size, int64_t offset);

#endif /* SERVER_ACK_H */
