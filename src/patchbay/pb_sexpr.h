#ifndef PB_SEXPR_H
#define PB_SEXPR_H

#include "pb_arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Borrowed text slice; ownership is determined by the containing value.
typedef struct pb_slice {
    const char *ptr;
    size_t len;
} pb_slice;

bool pb_slice_eq(pb_slice a, pb_slice b);
bool pb_slice_eq_lit(pb_slice s, const char *lit);

typedef enum pb_value_kind {
    PB_NIL,
    PB_BOOL,
    PB_NUMBER,
    PB_SYMBOL,
    PB_KEYWORD,
    PB_STRING,
    PB_LIST,
    PB_VECTOR,
} pb_value_kind;

typedef struct pb_value pb_value;

// Arena-owned sequence backing list and vector values.
typedef struct pb_values {
    pb_value *items;
    size_t len;
} pb_values;

// Patchbay AST node. Lists are call forms; vectors are data.
// tag is 0 until pb_resolve_rule runs. After it: a list's tag is its per-rule
// call-site id, a call head symbol's tag is form-table index + 1, and a bound
// symbol's tag names the binding. It lives in what was padding after kind.
struct pb_value {
    pb_value_kind kind;
    uint32_t tag;
    union {
        bool boolean;
        double number;
        pb_slice text;
        pb_values seq;
    };
};

_Static_assert(sizeof(pb_value) == 24, "pb_value tag must fit in kind's padding");

typedef enum pb_parse_error {
    PB_PARSE_OK,
    PB_PARSE_OOM,
    PB_PARSE_UNEXPECTED_EOF,
    PB_PARSE_UNEXPECTED_RPAREN,
    PB_PARSE_UNEXPECTED_RBRACKET,
    PB_PARSE_MISMATCHED_BRACKET,
    PB_PARSE_UNTERMINATED_STRING,
    PB_PARSE_INVALID_ESCAPE,
    PB_PARSE_INVALID_NUMBER,
    PB_PARSE_INVALID_LIST_HEAD,
    PB_PARSE_INVALID_JSON,
    PB_PARSE_INVALID_YAML,
} pb_parse_error;

// Parse result with top-level forms owned by the supplied arena.
typedef struct pb_parse_result {
    pb_parse_error err;
    size_t err_offset;
    pb_values forms;
    char err_msg[192]; // optional detail from the YAML and JSON adapters; empty when unused
} pb_parse_result;

pb_parse_result pb_parse_all(pb_arena *arena, const char *src, size_t len);
const char *pb_parse_error_name(pb_parse_error err);
// 1-based line and column of byte `offset` in src (clamped to len).
void pb_line_col(const char *src, size_t len, size_t offset, size_t *line, size_t *col);

#endif
