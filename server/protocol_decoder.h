/*
 * protocol_decoder.h — protocol decode module
 *
 * Handles the full decode pipeline from TCP byte stream to business messages:
 *   1. TCP framing: extract 4B BE length -> buffer complete frame
 *   2. Frame sync: verify "abc" magic (data frame) or skip (control frame)
 *   3. Compression: Zstd decompress
 *   4. Message: MsgPack decode -> {devno, offset, data}
 */

#ifndef SERVER_PROTOCOL_DECODER_H
#define SERVER_PROTOCOL_DECODER_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration — if session.h not yet included */
#ifndef SERVER_SESSION_H
struct session;
typedef struct session session_t;
#endif

/* Decode result type */
typedef enum {
    DECODE_DATA_BLOCK,
    DECODE_CTL_INCREMENTAL,
    DECODE_CTL_END_INCREMENTAL,
    DECODE_ERROR
} decode_result_t;

/* Decoded message */
typedef struct {
    decode_result_t type;
    int32_t  devno;
    int64_t  offset;
    uint8_t  data[1024 * 1024 + 4096];
    uint32_t data_len;
} decoded_msg_t;

/*
 * Feed newly received raw TCP data into the decoder.
 *
 * session: client session (maintains internal state machine)
 * buf:     newly received data
 * len:     data length
 * out:     decoded result (valid only when return == 1)
 *
 * Returns: 0 = need more data, 1 = decoded one message, -1 = decode error
 */
int protocol_decode(session_t *session, const uint8_t *buf, size_t len,
                    decoded_msg_t *out);

/* Error message string */
const char *decode_error_string(int err);

#endif /* SERVER_PROTOCOL_DECODER_H */
