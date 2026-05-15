#ifndef MB_PROTO_H
#define MB_PROTO_H

#include "buf.h"

#include <stddef.h>
#include <stdint.h>

enum {
    MB_MAX_CONTROL_LINE = 4096,
    MB_MAX_PAYLOAD = 1024 * 1024,
};

// Borrowed byte slice; callers must not retain it past the source buffer lifetime.
typedef struct mb_slice {
    const uint8_t *ptr;
    size_t len;
} mb_slice;

typedef enum mb_parse_status {
    MB_PARSE_OK,
    MB_PARSE_NEED_MORE,
    MB_PARSE_MALFORMED,
    MB_PARSE_UNKNOWN,
    MB_PARSE_INVALID_ARGS,
    MB_PARSE_PAYLOAD_TOO_LARGE,
    MB_PARSE_CONTROL_LINE_TOO_LONG,
} mb_parse_status;

typedef enum mb_op_kind {
    MB_OP_CONNECT,
    MB_OP_PING,
    MB_OP_SUB,
    MB_OP_UNSUB,
    MB_OP_PUB,
} mb_op_kind;

typedef struct mb_op {
    mb_op_kind kind;
    mb_slice subject;
    mb_slice queue;
    mb_slice sid;
    mb_slice payload;
    size_t max_msgs;
    bool has_max_msgs;
} mb_op;

typedef struct mb_parse_result {
    mb_parse_status status;
    mb_op op;
    size_t consumed;
} mb_parse_result;

typedef struct mb_info {
    const char *server_id;
    const char *host;
    const char *client_ip;
    unsigned int port;
    uint64_t client_id;
} mb_info;

mb_parse_result mb_parse_client_op(const uint8_t *buf, size_t len);
bool mb_write_pong(mb_buf *out);
bool mb_write_info(mb_buf *out, const mb_info *info);
bool mb_write_err(mb_buf *out, const char *msg);
bool mb_write_msg(mb_buf *out, mb_slice subject, mb_slice sid, mb_slice payload);
bool mb_write_msg_prefixed(mb_buf *out, const char *prefix, size_t prefix_len,
                           mb_slice subject, mb_slice sid, mb_slice payload);

#endif
