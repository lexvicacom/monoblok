#include "proto.h"

#include "mb_version.h"

#include <ctype.h>
#include <string.h>

#define APPEND_LIT(out, lit) mb_buf_append((out), (lit), sizeof(lit) - 1)

static bool eq_upper(mb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    if (s.len != n) {
        return false;
    }
    for (size_t i = 0; i < n; i += 1) {
        if ((char)toupper((unsigned char)s.ptr[i]) != lit[i]) {
            return false;
        }
    }
    return true;
}

static mb_slice trim_spaces(mb_slice s) {
    while (s.len > 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr += 1;
        s.len -= 1;
    }
    while (s.len > 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t')) {
        s.len -= 1;
    }
    return s;
}

static bool next_token(mb_slice *rest, mb_slice *tok) {
    *rest = trim_spaces(*rest);
    if (rest->len == 0) {
        return false;
    }
    size_t i = 0;
    while (i < rest->len && rest->ptr[i] != ' ' && rest->ptr[i] != '\t') {
        i += 1;
    }
    *tok = (mb_slice){.ptr = rest->ptr, .len = i};
    rest->ptr += i;
    rest->len -= i;
    return true;
}

static bool parse_usize(mb_slice s, size_t *out) {
    if (s.len == 0) {
        return false;
    }
    size_t v = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') {
            return false;
        }
        const size_t digit = (size_t)(s.ptr[i] - '0');
        if (v > (SIZE_MAX - digit) / 10) {
            return false;
        }
        v = v * 10 + digit;
    }
    *out = v;
    return true;
}

static mb_parse_result parse_sub(mb_slice rest, size_t consumed) {
    mb_slice subject = {0};
    mb_slice sid = {0};
    mb_slice extra = {0};
    if (!next_token(&rest, &subject) || !next_token(&rest, &sid)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }
    if (next_token(&rest, &extra)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }
    return (mb_parse_result){
        .status = MB_PARSE_OK,
        .op = {.kind = MB_OP_SUB, .subject = subject, .sid = sid},
        .consumed = consumed,
    };
}

static mb_parse_result parse_unsub(mb_slice rest, size_t consumed) {
    mb_slice sid = {0};
    mb_slice max_msgs = {0};
    mb_slice extra = {0};
    if (!next_token(&rest, &sid)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }

    mb_op op = {.kind = MB_OP_UNSUB, .sid = sid};
    if (next_token(&rest, &max_msgs)) {
        if (!parse_usize(max_msgs, &op.max_msgs)) {
            return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
        }
        op.has_max_msgs = true;
    }
    if (next_token(&rest, &extra)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }
    return (mb_parse_result){
        .status = MB_PARSE_OK,
        .op = op,
        .consumed = consumed,
    };
}

static mb_parse_result parse_pub(const uint8_t *buf, size_t len, mb_slice rest, size_t line_end) {
    mb_slice subject = {0};
    mb_slice bytes_str = {0};
    mb_slice extra = {0};
    if (!next_token(&rest, &subject) || !next_token(&rest, &bytes_str)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }
    if (next_token(&rest, &extra)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }

    size_t payload_len = 0;
    if (!parse_usize(bytes_str, &payload_len)) {
        return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
    }
    if (payload_len > MB_MAX_PAYLOAD) {
        return (mb_parse_result){.status = MB_PARSE_PAYLOAD_TOO_LARGE};
    }
    if (line_end > len) {
        return (mb_parse_result){.status = MB_PARSE_NEED_MORE};
    }

    const size_t available = len - line_end;
    if (available <= payload_len) {
        return (mb_parse_result){.status = MB_PARSE_NEED_MORE};
    }

    const uint8_t *payload = buf + line_end;
    const uint8_t *tail = payload + payload_len;
    size_t trailer_len = 0;
    if (tail[0] == '\n') {
        trailer_len = 1;
    } else if (tail[0] == '\r') {
        if (available < payload_len || available - payload_len < 2) {
            return (mb_parse_result){.status = MB_PARSE_NEED_MORE};
        }
        if (tail[1] != '\n') {
            return (mb_parse_result){.status = MB_PARSE_MALFORMED};
        }
        trailer_len = 2;
    } else {
        return (mb_parse_result){.status = MB_PARSE_MALFORMED};
    }

    if (payload_len > SIZE_MAX - line_end || trailer_len > SIZE_MAX - line_end - payload_len) {
        return (mb_parse_result){.status = MB_PARSE_PAYLOAD_TOO_LARGE};
    }

    return (mb_parse_result){
        .status = MB_PARSE_OK,
        .op = {
            .kind = MB_OP_PUB,
            .subject = subject,
            .payload = {.ptr = payload, .len = payload_len},
        },
        .consumed = line_end + payload_len + trailer_len,
    };
}

mb_parse_result mb_parse_client_op(const uint8_t *buf, size_t len) {
    size_t nl = 0;
    while (nl < len && buf[nl] != '\n') {
        nl += 1;
    }
    if (nl == len) {
        if (len > MB_MAX_CONTROL_LINE) {
            return (mb_parse_result){.status = MB_PARSE_CONTROL_LINE_TOO_LONG};
        }
        return (mb_parse_result){.status = MB_PARSE_NEED_MORE};
    }
    if (nl > MB_MAX_CONTROL_LINE) {
        return (mb_parse_result){.status = MB_PARSE_CONTROL_LINE_TOO_LONG};
    }

    const size_t line_end = nl + 1;
    size_t trimmed_len = nl;
    if (trimmed_len > 0 && buf[trimmed_len - 1] == '\r') {
        trimmed_len -= 1;
    }
    mb_slice line = {.ptr = buf, .len = trimmed_len};
    line = trim_spaces(line);
    if (line.len == 0) {
        return (mb_parse_result){.status = MB_PARSE_MALFORMED};
    }

    mb_slice rest = line;
    mb_slice verb = {0};
    if (!next_token(&rest, &verb)) {
        return (mb_parse_result){.status = MB_PARSE_MALFORMED};
    }
    if (eq_upper(verb, "PING")) {
        rest = trim_spaces(rest);
        if (rest.len != 0) {
            return (mb_parse_result){.status = MB_PARSE_INVALID_ARGS};
        }
        return (mb_parse_result){
            .status = MB_PARSE_OK,
            .op = {.kind = MB_OP_PING},
            .consumed = line_end,
        };
    }
    if (eq_upper(verb, "CONNECT")) {
        return (mb_parse_result){
            .status = MB_PARSE_OK,
            .op = {.kind = MB_OP_CONNECT},
            .consumed = line_end,
        };
    }
    if (eq_upper(verb, "SUB")) {
        return parse_sub(rest, line_end);
    }
    if (eq_upper(verb, "UNSUB")) {
        return parse_unsub(rest, line_end);
    }
    if (eq_upper(verb, "PUB")) {
        return parse_pub(buf, len, rest, line_end);
    }
    return (mb_parse_result){.status = MB_PARSE_UNKNOWN};
}

static bool append_decimal(mb_buf *out, size_t n) {
    char tmp[32];
    size_t i = sizeof tmp;
    if (n == 0) {
        return mb_buf_append_byte(out, '0');
    }
    while (n != 0) {
        i -= 1;
        tmp[i] = (char)('0' + (n % 10));
        n /= 10;
    }
    return mb_buf_append(out, tmp + i, sizeof tmp - i);
}

static bool append_u64_decimal(mb_buf *out, uint64_t n) {
    char tmp[32];
    size_t i = sizeof tmp;
    if (n == 0) {
        return mb_buf_append_byte(out, '0');
    }
    while (n != 0) {
        i -= 1;
        tmp[i] = (char)('0' + (n % 10));
        n /= 10;
    }
    return mb_buf_append(out, tmp + i, sizeof tmp - i);
}

static bool append_json_string(mb_buf *out, const char *s) {
    if (!mb_buf_append_byte(out, '\"')) {
        return false;
    }
    for (size_t i = 0; s[i] != '\0'; i += 1) {
        const char c = s[i];
        if (c == '\"' || c == '\\') {
            if (!mb_buf_append_byte(out, '\\') || !mb_buf_append_byte(out, (uint8_t)c)) {
                return false;
            }
        } else if ((unsigned char)c < 0x20) {
            if (!APPEND_LIT(out, "\\u00")) {
                return false;
            }
            static const char hex[] = "0123456789abcdef";
            if (!mb_buf_append_byte(out, (uint8_t)hex[((unsigned char)c) >> 4]) ||
                !mb_buf_append_byte(out, (uint8_t)hex[((unsigned char)c) & 0xf])) {
                return false;
            }
        } else if (!mb_buf_append_byte(out, (uint8_t)c)) {
            return false;
        }
    }
    return mb_buf_append_byte(out, '\"');
}

bool mb_write_pong(mb_buf *out) {
    return APPEND_LIT(out, "PONG\r\n");
}

bool mb_write_info(mb_buf *out, const mb_info *info) {
    return APPEND_LIT(out, "INFO {\"server_id\":") &&
           append_json_string(out, info->server_id) &&
           APPEND_LIT(out, ",\"server_name\":") &&
           append_json_string(out, info->server_id) &&
           APPEND_LIT(out, ",\"version\":") &&
           append_json_string(out, MB_VERSION) &&
           APPEND_LIT(out, ",\"proto\":1,\"headers\":false,\"max_payload\":1048576") &&
           APPEND_LIT(out, ",\"host\":") &&
           append_json_string(out, info->host) &&
           APPEND_LIT(out, ",\"port\":") &&
           append_decimal(out, info->port) &&
           APPEND_LIT(out, ",\"client_id\":") &&
           append_u64_decimal(out, info->client_id) &&
           APPEND_LIT(out, ",\"client_ip\":") &&
           append_json_string(out, info->client_ip) &&
           APPEND_LIT(out, ",\"build\":") &&
           append_json_string(out, MB_BUILD_INFO) &&
           APPEND_LIT(out, "}\r\n");
}

bool mb_write_err(mb_buf *out, const char *msg) {
    return mb_buf_append(out, "-ERR '", 6) &&
           mb_buf_append(out, msg, strlen(msg)) &&
           mb_buf_append(out, "'\r\n", 3);
}

bool mb_write_msg(mb_buf *out, mb_slice subject, mb_slice sid, mb_slice payload) {
    return mb_write_msg_prefixed(out, "", 0, subject, sid, payload);
}

bool mb_write_msg_prefixed(mb_buf *out, const char *prefix, size_t prefix_len,
                           mb_slice subject, mb_slice sid, mb_slice payload) {
    return mb_buf_append(out, "MSG ", 4) &&
           mb_buf_append(out, prefix, prefix_len) &&
           mb_buf_append(out, subject.ptr, subject.len) &&
           mb_buf_append_byte(out, ' ') &&
           mb_buf_append(out, sid.ptr, sid.len) &&
           mb_buf_append_byte(out, ' ') &&
           append_decimal(out, payload.len) &&
           mb_buf_append(out, "\r\n", 2) &&
           mb_buf_append(out, payload.ptr, payload.len) &&
           mb_buf_append(out, "\r\n", 2);
}
