#include "pb_yaml.h"

#include "array.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef enum y_node_kind {
    Y_SCALAR,
    Y_LIST,
    Y_MAP,
} y_node_kind;

typedef enum y_scalar_kind {
    Y_TEXT,
    Y_NUMBER,
    Y_BOOL,
    Y_NULL,
} y_scalar_kind;

typedef struct y_node y_node;

// YAML map entry in the temporary parse tree.
typedef struct y_pair {
    pb_slice key;
    y_node *value;
} y_pair;

// Temporary YAML subset node; freed after lowering into pb_value.
struct y_node {
    y_node_kind kind;
    size_t offset;
    y_scalar_kind scalar_kind;
    pb_slice text;
    double number;
    bool boolean;
    y_node **items;
    size_t len;
    size_t cap;
    y_pair *pairs;
    size_t pair_len;
    size_t pair_cap;
};

// Non-empty logical YAML line after comment and trailing-space trimming.
typedef struct y_line {
    size_t indent;
    const char *ptr;
    size_t len;
    size_t offset;
} y_line;

typedef struct y_parse {
    const char *src;
    size_t len;
    y_line *lines;
    size_t line_len;
    size_t line_cap;
    size_t pos;
    pb_parse_error err;
    size_t err_offset;
} y_parse;

typedef struct pb_build_vec {
    pb_value *items;
    size_t len;
    size_t cap;
} pb_build_vec;

static pb_slice trim_slice(pb_slice s) {
    while (s.len != 0 && (s.ptr[0] == ' ' || s.ptr[0] == '\t')) {
        s.ptr += 1;
        s.len -= 1;
    }
    while (s.len != 0 && (s.ptr[s.len - 1] == ' ' || s.ptr[s.len - 1] == '\t')) {
        s.len -= 1;
    }
    return s;
}

static bool slice_eq_lit(pb_slice s, const char *lit) {
    return pb_slice_eq_lit(s, lit);
}

static void set_err(y_parse *p, size_t offset) {
    if (p->err == PB_PARSE_OK) {
        p->err = PB_PARSE_INVALID_YAML;
        p->err_offset = offset;
    }
}

static y_node *node_new(y_parse *p, y_node_kind kind, size_t offset) {
    y_node *n = calloc(1, sizeof *n);
    if (n == NULL) {
        if (p->err == PB_PARSE_OK) {
            p->err = PB_PARSE_OOM;
            p->err_offset = offset;
        }
        return NULL;
    }
    n->kind = kind;
    n->offset = offset;
    return n;
}

static void node_free(y_node *n) {
    if (n == NULL) {
        return;
    }
    for (size_t i = 0; i < n->len; i += 1) {
        node_free(n->items[i]);
    }
    for (size_t i = 0; i < n->pair_len; i += 1) {
        node_free(n->pairs[i].value);
    }
    free(n->items);
    free(n->pairs);
    free(n);
}

static bool node_list_push(y_parse *p, y_node *list, y_node *item) {
    if (!mb_array_reserve((void **)&list->items, &list->cap, list->len + 1, sizeof list->items[0], 8)) {
        p->err = PB_PARSE_OOM;
        p->err_offset = item != NULL ? item->offset : list->offset;
        return false;
    }
    list->items[list->len] = item;
    list->len += 1;
    return true;
}

static bool node_map_put(y_parse *p, y_node *map, pb_slice key, y_node *value) {
    if (key.len == 0 || value == NULL) {
        set_err(p, value != NULL ? value->offset : map->offset);
        return false;
    }
    if (!mb_array_reserve((void **)&map->pairs, &map->pair_cap, map->pair_len + 1, sizeof map->pairs[0], 8)) {
        p->err = PB_PARSE_OOM;
        p->err_offset = value->offset;
        return false;
    }
    map->pairs[map->pair_len] = (y_pair){.key = key, .value = value};
    map->pair_len += 1;
    return true;
}

static bool line_vec_push(y_parse *p, y_line line) {
    if (!mb_array_reserve((void **)&p->lines, &p->line_cap, p->line_len + 1, sizeof p->lines[0], 16)) {
        p->err = PB_PARSE_OOM;
        p->err_offset = line.offset;
        return false;
    }
    p->lines[p->line_len] = line;
    p->line_len += 1;
    return true;
}

static size_t strip_comment_len(const char *ptr, size_t len) {
    char quote = '\0';
    size_t flow_depth = 0;
    for (size_t i = 0; i < len; i += 1) {
        const char c = ptr[i];
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else if (c == '\\' && quote == '"' && i + 1 < len) {
                i += 1;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            flow_depth += 1;
        } else if (c == ']' && flow_depth != 0) {
            flow_depth -= 1;
        } else if (c == '#' && flow_depth == 0) {
            return i;
        }
    }
    return len;
}

static bool collect_lines(y_parse *p) {
    size_t start = 0;
    while (start <= p->len) {
        size_t end = start;
        while (end < p->len && p->src[end] != '\n') {
            end += 1;
        }
        size_t line_end = end;
        if (line_end > start && p->src[line_end - 1] == '\r') {
            line_end -= 1;
        }
        const size_t raw_len = line_end - start;
        size_t indent = 0;
        while (indent < raw_len && p->src[start + indent] == ' ') {
            indent += 1;
        }
        if (indent < raw_len && p->src[start + indent] == '\t') {
            set_err(p, start + indent);
            return false;
        }
        const char *content = p->src + start + indent;
        size_t content_len = raw_len - indent;
        content_len = strip_comment_len(content, content_len);
        while (content_len != 0 && (content[content_len - 1] == ' ' || content[content_len - 1] == '\t')) {
            content_len -= 1;
        }
        if (content_len != 0 && !line_vec_push(p, (y_line){.indent = indent, .ptr = content, .len = content_len, .offset = start + indent})) {
            return false;
        }
        if (end == p->len) {
            break;
        }
        start = end + 1;
    }
    return true;
}

static bool line_is_seq(y_line line) {
    return line.len >= 1 && line.ptr[0] == '-' && (line.len == 1 || line.ptr[1] == ' ');
}

static bool simple_key(pb_slice key) {
    if (key.len == 0) {
        return false;
    }
    for (size_t i = 0; i < key.len; i += 1) {
        const unsigned char c = (unsigned char)key.ptr[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool find_key_colon(pb_slice s, size_t *out) {
    char quote = '\0';
    size_t flow_depth = 0;
    for (size_t i = 0; i < s.len; i += 1) {
        const char c = s.ptr[i];
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
            } else if (c == '\\' && quote == '"' && i + 1 < s.len) {
                i += 1;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '[') {
            flow_depth += 1;
        } else if (c == ']' && flow_depth != 0) {
            flow_depth -= 1;
        } else if (c == ':' && flow_depth == 0 && (i + 1 == s.len || s.ptr[i + 1] == ' ' || s.ptr[i + 1] == '\t')) {
            pb_slice key = trim_slice((pb_slice){.ptr = s.ptr, .len = i});
            if (!simple_key(key)) {
                return false;
            }
            *out = i;
            return true;
        }
    }
    return false;
}

static y_node *parse_block(y_parse *p, size_t indent);

static y_node *scalar_from_slice(y_parse *p, pb_slice raw, size_t offset) {
    raw = trim_slice(raw);
    y_node *n = node_new(p, Y_SCALAR, offset);
    if (n == NULL) {
        return NULL;
    }
    n->scalar_kind = Y_TEXT;
    if (raw.len >= 2 && ((raw.ptr[0] == '"' && raw.ptr[raw.len - 1] == '"') ||
                         (raw.ptr[0] == '\'' && raw.ptr[raw.len - 1] == '\''))) {
        n->text = (pb_slice){.ptr = raw.ptr + 1, .len = raw.len - 2};
        return n;
    }
    if (slice_eq_lit(raw, "null") || slice_eq_lit(raw, "nil") || slice_eq_lit(raw, "~")) {
        n->scalar_kind = Y_NULL;
        return n;
    }
    if (slice_eq_lit(raw, "true") || slice_eq_lit(raw, "false")) {
        n->scalar_kind = Y_BOOL;
        n->boolean = slice_eq_lit(raw, "true");
        return n;
    }
    if (raw.len < 128) {
        char tmp[128];
        memcpy(tmp, raw.ptr, raw.len);
        tmp[raw.len] = '\0';
        errno = 0;
        char *end = NULL;
        const double d = strtod(tmp, &end);
        if (errno == 0 && end == tmp + raw.len) {
            n->scalar_kind = Y_NUMBER;
            n->number = d;
            return n;
        }
    }
    n->text = raw;
    return n;
}

static y_node *parse_flow_value(y_parse *p, pb_slice s, size_t *pos, size_t base_offset);

static void flow_skip_ws(pb_slice s, size_t *pos) {
    while (*pos < s.len && (s.ptr[*pos] == ' ' || s.ptr[*pos] == '\t')) {
        *pos += 1;
    }
}

static y_node *parse_flow_array(y_parse *p, pb_slice s, size_t *pos, size_t base_offset) {
    y_node *list = node_new(p, Y_LIST, base_offset + *pos);
    if (list == NULL) {
        return NULL;
    }
    *pos += 1;
    flow_skip_ws(s, pos);
    if (*pos < s.len && s.ptr[*pos] == ']') {
        *pos += 1;
        return list;
    }
    for (;;) {
        y_node *item = parse_flow_value(p, s, pos, base_offset);
        if (item == NULL || !node_list_push(p, list, item)) {
            node_free(item);
            node_free(list);
            return NULL;
        }
        flow_skip_ws(s, pos);
        if (*pos >= s.len) {
            set_err(p, base_offset + *pos);
            node_free(list);
            return NULL;
        }
        if (s.ptr[*pos] == ']') {
            *pos += 1;
            return list;
        }
        if (s.ptr[*pos] != ',') {
            set_err(p, base_offset + *pos);
            node_free(list);
            return NULL;
        }
        *pos += 1;
        flow_skip_ws(s, pos);
    }
}

static y_node *parse_flow_scalar(y_parse *p, pb_slice s, size_t *pos, size_t base_offset) {
    const size_t start = *pos;
    if (*pos < s.len && (s.ptr[*pos] == '"' || s.ptr[*pos] == '\'')) {
        const char quote = s.ptr[*pos];
        *pos += 1;
        while (*pos < s.len) {
            if (s.ptr[*pos] == quote) {
                *pos += 1;
                return scalar_from_slice(p, (pb_slice){.ptr = s.ptr + start, .len = *pos - start}, base_offset + start);
            }
            if (s.ptr[*pos] == '\\' && quote == '"' && *pos + 1 < s.len) {
                *pos += 2;
            } else {
                *pos += 1;
            }
        }
        set_err(p, base_offset + start);
        return NULL;
    }
    while (*pos < s.len && s.ptr[*pos] != ',' && s.ptr[*pos] != ']') {
        *pos += 1;
    }
    pb_slice raw = trim_slice((pb_slice){.ptr = s.ptr + start, .len = *pos - start});
    if (raw.len == 0) {
        set_err(p, base_offset + start);
        return NULL;
    }
    return scalar_from_slice(p, raw, base_offset + start);
}

static y_node *parse_flow_value(y_parse *p, pb_slice s, size_t *pos, size_t base_offset) {
    flow_skip_ws(s, pos);
    if (*pos < s.len && s.ptr[*pos] == '[') {
        return parse_flow_array(p, s, pos, base_offset);
    }
    return parse_flow_scalar(p, s, pos, base_offset);
}

static y_node *parse_inline_value(y_parse *p, pb_slice raw, size_t offset) {
    raw = trim_slice(raw);
    if (raw.len == 0) {
        set_err(p, offset);
        return NULL;
    }
    if (raw.ptr[0] == '[') {
        size_t pos = 0;
        y_node *n = parse_flow_array(p, raw, &pos, offset);
        flow_skip_ws(raw, &pos);
        if (n == NULL || pos != raw.len) {
            node_free(n);
            set_err(p, offset + pos);
            return NULL;
        }
        return n;
    }
    return scalar_from_slice(p, raw, offset);
}

static bool parse_map_pair_from_line(y_parse *p, y_node *map, y_line line, bool advance_line) {
    size_t colon = 0;
    if (!find_key_colon((pb_slice){.ptr = line.ptr, .len = line.len}, &colon)) {
        set_err(p, line.offset);
        return false;
    }
    pb_slice key = trim_slice((pb_slice){.ptr = line.ptr, .len = colon});
    pb_slice raw_value = trim_slice((pb_slice){.ptr = line.ptr + colon + 1, .len = line.len - colon - 1});
    y_node *value = NULL;
    if (raw_value.len != 0) {
        value = parse_inline_value(p, raw_value, line.offset + colon + 1);
        if (advance_line) {
            p->pos += 1;
        }
    } else {
        p->pos += 1;
        if (p->pos >= p->line_len || p->lines[p->pos].indent <= line.indent) {
            set_err(p, line.offset + colon);
            return false;
        }
        value = parse_block(p, p->lines[p->pos].indent);
    }
    if (value == NULL || !node_map_put(p, map, key, value)) {
        node_free(value);
        return false;
    }
    return true;
}

static y_node *parse_map(y_parse *p, size_t indent) {
    y_node *map = node_new(p, Y_MAP, p->lines[p->pos].offset);
    if (map == NULL) {
        return NULL;
    }
    while (p->pos < p->line_len && p->lines[p->pos].indent == indent && !line_is_seq(p->lines[p->pos])) {
        y_line line = p->lines[p->pos];
        if (!parse_map_pair_from_line(p, map, line, true)) {
            node_free(map);
            return NULL;
        }
    }
    if (map->pair_len == 0) {
        set_err(p, map->offset);
        node_free(map);
        return NULL;
    }
    return map;
}

static y_node *parse_seq_map_item(y_parse *p, y_line line, pb_slice rest, size_t colon, size_t indent) {
    y_node *map = node_new(p, Y_MAP, line.offset);
    if (map == NULL) {
        return NULL;
    }
    pb_slice key = trim_slice((pb_slice){.ptr = rest.ptr, .len = colon});
    pb_slice raw_value = trim_slice((pb_slice){.ptr = rest.ptr + colon + 1, .len = rest.len - colon - 1});
    y_node *value = NULL;
    if (raw_value.len != 0) {
        value = parse_inline_value(p, raw_value, line.offset + (size_t)(raw_value.ptr - line.ptr));
        p->pos += 1;
    } else {
        p->pos += 1;
        if (p->pos >= p->line_len || p->lines[p->pos].indent <= indent) {
            set_err(p, line.offset + (size_t)(rest.ptr - line.ptr) + colon);
            node_free(map);
            return NULL;
        }
        value = parse_block(p, p->lines[p->pos].indent);
    }
    if (value == NULL || !node_map_put(p, map, key, value)) {
        node_free(value);
        node_free(map);
        return NULL;
    }
    while (p->pos < p->line_len && p->lines[p->pos].indent > indent) {
        y_line child = p->lines[p->pos];
        if (child.indent != indent + 2 || line_is_seq(child)) {
            set_err(p, child.offset);
            node_free(map);
            return NULL;
        }
        if (!parse_map_pair_from_line(p, map, child, true)) {
            node_free(map);
            return NULL;
        }
    }
    return map;
}

static y_node *parse_seq(y_parse *p, size_t indent) {
    y_node *list = node_new(p, Y_LIST, p->lines[p->pos].offset);
    if (list == NULL) {
        return NULL;
    }
    while (p->pos < p->line_len && p->lines[p->pos].indent == indent && line_is_seq(p->lines[p->pos])) {
        y_line line = p->lines[p->pos];
        pb_slice rest = line.len <= 1 ? (pb_slice){0} : trim_slice((pb_slice){.ptr = line.ptr + 1, .len = line.len - 1});
        y_node *item = NULL;
        size_t colon = 0;
        if (rest.len == 0) {
            p->pos += 1;
            if (p->pos >= p->line_len || p->lines[p->pos].indent <= indent) {
                set_err(p, line.offset);
                node_free(list);
                return NULL;
            }
            item = parse_block(p, p->lines[p->pos].indent);
        } else if (find_key_colon(rest, &colon)) {
            item = parse_seq_map_item(p, line, rest, colon, indent);
        } else {
            item = parse_inline_value(p, rest, line.offset + (size_t)(rest.ptr - line.ptr));
            p->pos += 1;
        }
        if (item == NULL || !node_list_push(p, list, item)) {
            node_free(item);
            node_free(list);
            return NULL;
        }
    }
    if (list->len == 0) {
        set_err(p, list->offset);
        node_free(list);
        return NULL;
    }
    return list;
}

static y_node *parse_block(y_parse *p, size_t indent) {
    if (p->pos >= p->line_len || p->lines[p->pos].indent != indent) {
        set_err(p, p->pos < p->line_len ? p->lines[p->pos].offset : p->len);
        return NULL;
    }
    if (line_is_seq(p->lines[p->pos])) {
        return parse_seq(p, indent);
    }
    return parse_map(p, indent);
}

static y_pair *map_find(y_node *map, const char *key) {
    if (map == NULL || map->kind != Y_MAP) {
        return NULL;
    }
    for (size_t i = 0; i < map->pair_len; i += 1) {
        if (slice_eq_lit(map->pairs[i].key, key)) {
            return &map->pairs[i];
        }
    }
    return NULL;
}

static bool pb_vec_push(pb_build_vec *v, pb_value item) {
    if (!mb_array_reserve((void **)&v->items, &v->cap, v->len + 1, sizeof v->items[0], 8)) {
        return false;
    }
    v->items[v->len] = item;
    v->len += 1;
    return true;
}

static bool arena_text(pb_arena *arena, pb_value_kind kind, pb_slice text, pb_value *out) {
    char *owned = pb_arena_memdup(arena, text.ptr, text.len);
    if (owned == NULL) {
        return false;
    }
    *out = (pb_value){.kind = kind, .text = {.ptr = owned, .len = text.len}};
    return true;
}

static bool arena_text_lit(pb_arena *arena, pb_value_kind kind, const char *lit, pb_value *out) {
    return arena_text(arena, kind, (pb_slice){.ptr = lit, .len = strlen(lit)}, out);
}

static bool freeze_pb_vec(pb_arena *arena, pb_build_vec *v, pb_value_kind kind, pb_value *out) {
    pb_value *items = pb_arena_alloc(arena, v->len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && v->len != 0) {
        return false;
    }
    if (v->len != 0) {
        memcpy(items, v->items, v->len * sizeof items[0]);
    }
    *out = (pb_value){.kind = kind, .seq = {.items = items, .len = v->len}};
    return true;
}

static bool scalar_is_bound(pb_slice s) {
    return slice_eq_lit(s, "subject") ||
           slice_eq_lit(s, "payload") ||
           slice_eq_lit(s, "payload-float") ||
           slice_eq_lit(s, "payload-int");
}

static bool lower_scalar(pb_arena *arena, y_node *n, bool expr, pb_value *out) {
    if (n->scalar_kind == Y_NULL) {
        *out = (pb_value){.kind = PB_NIL};
        return true;
    }
    if (n->scalar_kind == Y_BOOL) {
        *out = (pb_value){.kind = PB_BOOL, .boolean = n->boolean};
        return true;
    }
    if (n->scalar_kind == Y_NUMBER) {
        *out = (pb_value){.kind = PB_NUMBER, .number = n->number};
        return true;
    }
    if (n->text.len > 1 && n->text.ptr[0] == ':') {
        return arena_text(arena, PB_KEYWORD, (pb_slice){.ptr = n->text.ptr + 1, .len = n->text.len - 1}, out);
    }
    return arena_text(arena, expr && scalar_is_bound(n->text) ? PB_SYMBOL : PB_STRING, n->text, out);
}

static bool lower_expr(pb_arena *arena, y_node *n, pb_value *out);
static bool lower_body(pb_arena *arena, y_node *n, pb_value *out);

static bool lower_call(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind != Y_LIST || n->len == 0 || n->items[0]->kind != Y_SCALAR || n->items[0]->scalar_kind != Y_TEXT) {
        return false;
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    if (!arena_text(arena, PB_SYMBOL, n->items[0]->text, &head) || !pb_vec_push(&v, head)) {
        free(v.items);
        return false;
    }
    for (size_t i = 1; i < n->len; i += 1) {
        pb_value item = {0};
        if (!lower_expr(arena, n->items[i], &item) || !pb_vec_push(&v, item)) {
            free(v.items);
            return false;
        }
    }
    const bool ok = freeze_pb_vec(arena, &v, PB_LIST, out);
    free(v.items);
    return ok;
}

static bool lower_thread(pb_arena *arena, y_node *n, pb_value *out) {
    y_node *from = NULL;
    y_node *steps = NULL;
    if (n->kind == Y_LIST) {
        if (n->len == 0) {
            return false;
        }
        from = n->items[0];
    } else if (n->kind == Y_MAP) {
        y_pair *from_pair = map_find(n, "from");
        y_pair *steps_pair = map_find(n, "steps");
        if (from_pair == NULL || steps_pair == NULL || steps_pair->value->kind != Y_LIST) {
            return false;
        }
        from = from_pair->value;
        steps = steps_pair->value;
    } else {
        return false;
    }

    pb_build_vec v = {0};
    pb_value head = {0};
    pb_value seed = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, "->", &head) ||
        !pb_vec_push(&v, head) ||
        !lower_expr(arena, from, &seed) ||
        !pb_vec_push(&v, seed)) {
        free(v.items);
        return false;
    }
    if (steps == NULL) {
        steps = n;
        for (size_t i = 1; i < steps->len; i += 1) {
            pb_value step = {0};
            if (!lower_expr(arena, steps->items[i], &step) || !pb_vec_push(&v, step)) {
                free(v.items);
                return false;
            }
        }
    } else {
        for (size_t i = 0; i < steps->len; i += 1) {
            pb_value step = {0};
            if (!lower_expr(arena, steps->items[i], &step) || !pb_vec_push(&v, step)) {
                free(v.items);
                return false;
            }
        }
    }
    const bool ok = freeze_pb_vec(arena, &v, PB_LIST, out);
    free(v.items);
    return ok;
}

static bool lower_when(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind != Y_MAP) {
        return false;
    }
    y_pair *test_pair = map_find(n, "test");
    y_pair *then_pair = map_find(n, "then");
    if (test_pair == NULL || then_pair == NULL) {
        return false;
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    pb_value test = {0};
    pb_value then_body = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, "when", &head) ||
        !pb_vec_push(&v, head) ||
        !lower_expr(arena, test_pair->value, &test) ||
        !pb_vec_push(&v, test) ||
        !lower_body(arena, then_pair->value, &then_body) ||
        !pb_vec_push(&v, then_body)) {
        free(v.items);
        return false;
    }
    const bool ok = freeze_pb_vec(arena, &v, PB_LIST, out);
    free(v.items);
    return ok;
}

static bool lower_do(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind != Y_LIST) {
        return false;
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, "do", &head) || !pb_vec_push(&v, head)) {
        free(v.items);
        return false;
    }
    for (size_t i = 0; i < n->len; i += 1) {
        pb_value item = {0};
        if (!lower_body(arena, n->items[i], &item) || !pb_vec_push(&v, item)) {
            free(v.items);
            return false;
        }
    }
    const bool ok = freeze_pb_vec(arena, &v, PB_LIST, out);
    free(v.items);
    return ok;
}

static bool lower_body(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind == Y_MAP) {
        y_pair *p = NULL;
        if ((p = map_find(n, "thread")) != NULL) {
            return lower_thread(arena, p->value, out);
        }
        if ((p = map_find(n, "when")) != NULL) {
            return lower_when(arena, p->value, out);
        }
        if ((p = map_find(n, "do")) != NULL) {
            return lower_do(arena, p->value, out);
        }
        if ((p = map_find(n, "form")) != NULL || (p = map_find(n, "body")) != NULL) {
            return lower_expr(arena, p->value, out);
        }
        return false;
    }
    return lower_expr(arena, n, out);
}

static bool lower_expr(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind == Y_SCALAR) {
        return lower_scalar(arena, n, true, out);
    }
    if (n->kind == Y_LIST) {
        return lower_call(arena, n, out);
    }
    return lower_body(arena, n, out);
}

static bool lower_config_value(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind == Y_LIST) {
        pb_build_vec v = {0};
        for (size_t i = 0; i < n->len; i += 1) {
            pb_value item = {0};
            if (!lower_config_value(arena, n->items[i], &item) || !pb_vec_push(&v, item)) {
                free(v.items);
                return false;
            }
        }
        const bool ok = freeze_pb_vec(arena, &v, PB_VECTOR, out);
        free(v.items);
        return ok;
    }
    if (n->kind != Y_SCALAR) {
        return false;
    }
    return lower_scalar(arena, n, false, out);
}

static bool lower_lvc(pb_arena *arena, y_node *n, pb_value *out) {
    pb_build_vec form = {0};
    pb_value head = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, "lvc", &head) || !pb_vec_push(&form, head)) {
        free(form.items);
        return false;
    }
    if (n->kind == Y_LIST) {
        pb_value vec = {0};
        if (!lower_config_value(arena, n, &vec) || !pb_vec_push(&form, vec)) {
            free(form.items);
            return false;
        }
    } else {
        pb_value one = {0};
        if (!lower_config_value(arena, n, &one) || !pb_vec_push(&form, one)) {
            free(form.items);
            return false;
        }
    }
    const bool ok = freeze_pb_vec(arena, &form, PB_LIST, out);
    free(form.items);
    return ok;
}

static bool lower_config_form(pb_arena *arena, const char *name, y_node *n, pb_value *out) {
    if (n->kind != Y_MAP) {
        return false;
    }
    pb_build_vec form = {0};
    pb_value head = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, name, &head) || !pb_vec_push(&form, head)) {
        free(form.items);
        return false;
    }
    for (size_t i = 0; i < n->pair_len; i += 1) {
        pb_value key = {0};
        pb_value value = {0};
        if (!arena_text(arena, PB_KEYWORD, n->pairs[i].key, &key) ||
            !lower_config_value(arena, n->pairs[i].value, &value) ||
            !pb_vec_push(&form, key) ||
            !pb_vec_push(&form, value)) {
            free(form.items);
            return false;
        }
    }
    const bool ok = freeze_pb_vec(arena, &form, PB_LIST, out);
    free(form.items);
    return ok;
}

static bool lower_on_rule(pb_arena *arena, y_node *n, pb_value *out) {
    if (n->kind != Y_MAP) {
        return false;
    }
    y_pair *sub = map_find(n, "sub");
    if (sub == NULL || sub->value->kind != Y_SCALAR || sub->value->scalar_kind != Y_TEXT) {
        return false;
    }

    pb_build_vec form = {0};
    pb_value head = {0};
    pb_value filter = {0};
    if (!arena_text_lit(arena, PB_SYMBOL, "on", &head) ||
        !pb_vec_push(&form, head) ||
        !arena_text(arena, PB_STRING, sub->value->text, &filter) ||
        !pb_vec_push(&form, filter)) {
        free(form.items);
        return false;
    }

    y_pair *reentrant = map_find(n, "reentrant");
    if (reentrant != NULL) {
        if (reentrant->value->kind != Y_SCALAR || reentrant->value->scalar_kind != Y_BOOL) {
            free(form.items);
            return false;
        }
        pb_value key = {0};
        pb_value value = {.kind = PB_BOOL, .boolean = reentrant->value->boolean};
        if (!arena_text_lit(arena, PB_KEYWORD, "reentrant", &key) ||
            !pb_vec_push(&form, key) ||
            !pb_vec_push(&form, value)) {
            free(form.items);
            return false;
        }
    }

    pb_value body = {0};
    if (!lower_body(arena, n, &body) || !pb_vec_push(&form, body)) {
        free(form.items);
        return false;
    }
    const bool ok = freeze_pb_vec(arena, &form, PB_LIST, out);
    free(form.items);
    return ok;
}

static bool lower_on(pb_arena *arena, y_node *n, pb_build_vec *forms) {
    if (n->kind != Y_LIST) {
        return false;
    }
    for (size_t i = 0; i < n->len; i += 1) {
        pb_value form = {0};
        if (!lower_on_rule(arena, n->items[i], &form) || !pb_vec_push(forms, form)) {
            return false;
        }
    }
    return true;
}

static bool lower_root(pb_arena *arena, y_node *root, pb_values *out) {
    if (root->kind != Y_MAP) {
        return false;
    }
    pb_build_vec forms = {0};
    for (size_t i = 0; i < root->pair_len; i += 1) {
        y_pair *p = &root->pairs[i];
        if (slice_eq_lit(p->key, "on")) {
            if (!lower_on(arena, p->value, &forms)) {
                free(forms.items);
                return false;
            }
        } else if (slice_eq_lit(p->key, "lvc")) {
            pb_value form = {0};
            if (!lower_lvc(arena, p->value, &form) || !pb_vec_push(&forms, form)) {
                free(forms.items);
                return false;
            }
        } else if (slice_eq_lit(p->key, "export") || slice_eq_lit(p->key, "bridge") || slice_eq_lit(p->key, "import")) {
            char name[16];
            if (p->key.len >= sizeof name) {
                free(forms.items);
                return false;
            }
            memcpy(name, p->key.ptr, p->key.len);
            name[p->key.len] = '\0';
            pb_value form = {0};
            if (!lower_config_form(arena, name, p->value, &form) || !pb_vec_push(&forms, form)) {
                free(forms.items);
                return false;
            }
        } else {
            free(forms.items);
            return false;
        }
    }
    pb_value *items = pb_arena_alloc(arena, forms.len * sizeof items[0], _Alignof(pb_value));
    if (items == NULL && forms.len != 0) {
        free(forms.items);
        return false;
    }
    if (forms.len != 0) {
        memcpy(items, forms.items, forms.len * sizeof items[0]);
    }
    *out = (pb_values){.items = items, .len = forms.len};
    free(forms.items);
    return true;
}

pb_parse_result pb_parse_yaml_patchbay(pb_arena *arena, const char *src, size_t len) {
    y_parse p = {.src = src, .len = len, .err = PB_PARSE_OK};
    if (!collect_lines(&p)) {
        free(p.lines);
        return (pb_parse_result){.err = p.err, .err_offset = p.err_offset};
    }
    if (p.line_len == 0) {
        free(p.lines);
        return (pb_parse_result){.err = PB_PARSE_OK};
    }
    y_node *root = parse_block(&p, p.lines[0].indent);
    if (p.err == PB_PARSE_OK && p.pos != p.line_len) {
        set_err(&p, p.lines[p.pos].offset);
    }
    pb_values forms = {0};
    if (p.err == PB_PARSE_OK && !lower_root(arena, root, &forms)) {
        p.err = PB_PARSE_INVALID_YAML;
        p.err_offset = root != NULL ? root->offset : 0;
    }
    node_free(root);
    free(p.lines);
    if (p.err != PB_PARSE_OK) {
        return (pb_parse_result){.err = p.err, .err_offset = p.err_offset};
    }
    return (pb_parse_result){.err = PB_PARSE_OK, .forms = forms};
}
