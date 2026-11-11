#include "pb_yaml.h"

#include "array.h"
#include "pb_eval_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
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
    bool quoted; // text came from a quoted scalar: always a string
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

// Temporary growable value list; freed after freezing into the arena.
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
        n->quoted = true;
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

// Lowering state: arena for output values plus the first error with its detail message.
typedef struct y_lower {
    pb_arena *arena;
    pb_parse_error err;
    size_t err_offset;
    char msg[192];
} y_lower;

static bool lower_fail(y_lower *L, size_t offset, const char *fmt, ...) {
    if (L->err == PB_PARSE_OK) {
        L->err = PB_PARSE_INVALID_YAML;
        L->err_offset = offset;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(L->msg, sizeof L->msg, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool lower_oom(y_lower *L, size_t offset) {
    if (L->err == PB_PARSE_OK) {
        L->err = PB_PARSE_OOM;
        L->err_offset = offset;
    }
    return false;
}

static bool vec_push(y_lower *L, pb_build_vec *v, pb_value item) {
    if (!mb_array_reserve((void **)&v->items, &v->cap, v->len + 1, sizeof v->items[0], 8)) {
        return lower_oom(L, 0);
    }
    v->items[v->len] = item;
    v->len += 1;
    return true;
}

static bool arena_text(y_lower *L, pb_value_kind kind, pb_slice text, pb_value *out) {
    char *owned = pb_arena_memdup(L->arena, text.ptr, text.len);
    if (owned == NULL) {
        return lower_oom(L, 0);
    }
    *out = (pb_value){.kind = kind, .text = {.ptr = owned, .len = text.len}};
    return true;
}

static bool arena_text_lit(y_lower *L, pb_value_kind kind, const char *lit, pb_value *out) {
    return arena_text(L, kind, (pb_slice){.ptr = lit, .len = strlen(lit)}, out);
}

static bool freeze_pb_vec(y_lower *L, pb_build_vec *v, pb_value_kind kind, pb_value *out) {
    pb_value *items = pb_arena_alloc(L->arena, v->len * sizeof items[0], alignof(pb_value));
    if (items == NULL && v->len != 0) {
        return lower_oom(L, 0);
    }
    if (v->len != 0) {
        memcpy(items, v->items, v->len * sizeof items[0]);
    }
    *out = (pb_value){.kind = kind, .seq = {.items = items, .len = v->len}};
    return true;
}

// Frees the scratch vector and reports whether lowering is still healthy.
static bool vec_done(pb_build_vec *v, bool ok) {
    free(v->items);
    v->items = NULL;
    return ok;
}

// Keyword option words allowed unquoted in YAML rule bodies; they lower to :word.
// Keep in sync with the keyword comparisons in the pb_form_*.c implementations.
static const char *const OPTION_WORDS[] = {
    "ms", "dp", "subject", "lost", "found", "leaf", "date", "hour", "minute",
    "avg", "sum", "min", "max", "count", "rate", "reentrant",
};

static bool is_option_word(pb_slice s) {
    for (size_t i = 0; i < sizeof OPTION_WORDS / sizeof OPTION_WORDS[0]; i += 1) {
        if (slice_eq_lit(s, OPTION_WORDS[i])) {
            return true;
        }
    }
    return false;
}

// Config positions (lvc, export, import): unquoted words are strings, ":word" is a keyword.
static bool lower_config_scalar(y_lower *L, y_node *n, pb_value *out) {
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
    if (!n->quoted && n->text.len > 1 && n->text.ptr[0] == ':') {
        return arena_text(L, PB_KEYWORD, (pb_slice){.ptr = n->text.ptr + 1, .len = n->text.len - 1}, out);
    }
    return arena_text(L, PB_STRING, n->text, out);
}

static bool word_is_binding(pb_slice w) {
    return pb_binding_lookup(w) != 0;
}

// Rule-body scalar: number, bool, null, quoted string, $binding, or option word.
static bool lower_expr_scalar(y_lower *L, y_node *n, pb_value *out) {
    if (n->scalar_kind != Y_TEXT) {
        return lower_config_scalar(L, n, out);
    }
    if (n->quoted) {
        return arena_text(L, PB_STRING, n->text, out);
    }
    const pb_slice w = n->text;
    if (w.len > 0 && w.ptr[0] == '$') {
        const pb_slice name = {.ptr = w.ptr + 1, .len = w.len - 1};
        // Older libyaml rejects `?` in flow scalars; keep a portable YAML alias.
        if (slice_eq_lit(name, "replaying")) {
            return arena_text_lit(L, PB_SYMBOL, "replaying?", out);
        }
        if (word_is_binding(name)) {
            return arena_text(L, PB_SYMBOL, name, out);
        }
        char list[160] = "";
        size_t used = 0;
        for (size_t i = 0; pb_binding_name(i) != NULL && used < sizeof list; i += 1) {
            used += (size_t)snprintf(list + used, sizeof list - used, "%s$%s", i == 0 ? "" : ", ", pb_binding_name(i));
        }
        return lower_fail(L, n->offset, "unknown binding $%.*s (known: %s)", (int)name.len, name.ptr, list);
    }
    if (w.len > 1 && w.ptr[0] == ':') {
        return lower_fail(L, n->offset, "keyword \"%.*s\" is not valid in YAML; write the option without the colon: %.*s (quote it to publish the text)",
                          (int)w.len, w.ptr, (int)w.len - 1, w.ptr + 1);
    }
    if (is_option_word(w)) {
        return arena_text(L, PB_KEYWORD, w, out);
    }
    if (word_is_binding(w)) {
        return lower_fail(L, n->offset, "unquoted word \"%.*s\" in a rule body; use $%.*s for the bound value, or quote it for the text",
                          (int)w.len, w.ptr, (int)w.len, w.ptr);
    }
    return lower_fail(L, n->offset, "unquoted word \"%.*s\" in a rule body; quote it to use it as text (\"%.*s\"), or fix the typo if it was meant as an option",
                      (int)w.len, w.ptr, (int)w.len, w.ptr);
}

static bool lower_expr(y_lower *L, y_node *n, pb_value *out);
static bool lower_body_map(y_lower *L, y_node *n, const char *rule_key, pb_value *out);

static bool lower_vector(y_lower *L, y_node *n, pb_value *out) {
    pb_build_vec v = {0};
    for (size_t i = 1; i < n->len; i += 1) {
        pb_value item = {0};
        if (!lower_expr(L, n->items[i], &item) || !vec_push(L, &v, item)) {
            return vec_done(&v, false);
        }
    }
    const bool ok = freeze_pb_vec(L, &v, PB_VECTOR, out);
    return vec_done(&v, ok);
}

static bool lower_call(y_lower *L, y_node *n, pb_value *out) {
    if (n->len == 0) {
        return lower_fail(L, n->offset, "empty list; the first item must be an operator name");
    }
    y_node *h = n->items[0];
    if (h->kind != Y_SCALAR || h->scalar_kind != Y_TEXT) {
        return lower_fail(L, h->offset, "the first item of a list must be an operator name (or the word vector)");
    }
    if (!h->quoted) {
        const pb_slice w = h->text;
        if (w.ptr[0] == '$') {
            return lower_fail(L, h->offset, "a bound value cannot be the first item of a list; the first item is the operator");
        }
        if (!isalpha((unsigned char)w.ptr[0])) {
            return lower_fail(L, h->offset, "operator \"%.*s\" must be quoted in YAML: [\"%.*s\", ...]", (int)w.len, w.ptr, (int)w.len, w.ptr);
        }
    }
    if (slice_eq_lit(h->text, "vector")) {
        return lower_vector(L, n, out);
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    if (!arena_text(L, PB_SYMBOL, h->text, &head) || !vec_push(L, &v, head)) {
        return vec_done(&v, false);
    }
    for (size_t i = 1; i < n->len; i += 1) {
        pb_value item = {0};
        // A bare `subject` is the :subject option, which only count! takes right
        // after the operator. As the last item an option has no value. The usual
        // mistake is [publish!, subject, ...] when $subject was meant.
        y_node *arg = n->items[i];
        if (arg->kind == Y_SCALAR && arg->scalar_kind == Y_TEXT && !arg->quoted && slice_eq_lit(arg->text, "subject") &&
            (i == n->len - 1 || (i == 1 && !slice_eq_lit(h->text, "count!")))) {
            vec_done(&v, false);
            return lower_fail(L, arg->offset, "bare \"subject\" here is the :subject option, which has no value in this position; use $subject for the bound value, or quote it for the text");
        }
        if (!lower_expr(L, n->items[i], &item) || !vec_push(L, &v, item)) {
            return vec_done(&v, false);
        }
    }
    const bool ok = freeze_pb_vec(L, &v, PB_LIST, out);
    return vec_done(&v, ok);
}

static bool lower_thread(y_lower *L, y_node *n, pb_value *out) {
    if (n->kind == Y_MAP) {
        return lower_fail(L, n->offset, "thread: the from/steps map form was removed; write a list whose first item is the start value and the rest are steps");
    }
    if (n->kind != Y_LIST || n->len == 0) {
        return lower_fail(L, n->offset, "thread must be a non-empty list: the start value, then the steps");
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    if (!arena_text_lit(L, PB_SYMBOL, "->", &head) || !vec_push(L, &v, head)) {
        return vec_done(&v, false);
    }
    for (size_t i = 0; i < n->len; i += 1) {
        pb_value item = {0};
        if (!lower_expr(L, n->items[i], &item) || !vec_push(L, &v, item)) {
            return vec_done(&v, false);
        }
    }
    const bool ok = freeze_pb_vec(L, &v, PB_LIST, out);
    return vec_done(&v, ok);
}

static bool lower_do(y_lower *L, y_node *n, pb_value *out) {
    if (n->kind != Y_LIST || n->len == 0) {
        return lower_fail(L, n->offset, "do must be a non-empty list of bodies");
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    if (!arena_text_lit(L, PB_SYMBOL, "do", &head) || !vec_push(L, &v, head)) {
        return vec_done(&v, false);
    }
    for (size_t i = 0; i < n->len; i += 1) {
        pb_value item = {0};
        const bool lowered = n->items[i]->kind == Y_MAP ? lower_body_map(L, n->items[i], NULL, &item)
                                                         : lower_expr(L, n->items[i], &item);
        if (!lowered || !vec_push(L, &v, item)) {
            return vec_done(&v, false);
        }
    }
    const bool ok = freeze_pb_vec(L, &v, PB_LIST, out);
    return vec_done(&v, ok);
}

// Expression position: scalars and flow arrays only; maps are bodies and live in body positions.
static bool lower_expr(y_lower *L, y_node *n, pb_value *out) {
    if (n->kind == Y_SCALAR) {
        return lower_expr_scalar(L, n, out);
    }
    if (n->kind == Y_LIST) {
        return lower_call(L, n, out);
    }
    return lower_fail(L, n->offset, "a map is not an expression; use a list such as [operator, args...]");
}

// A body map: optional `when` guard plus exactly one of thread, do, form. At rule level
// the caller owns reentrant and rule_key ("sub" for on, "ms" for every); nested bodies pass NULL.
static bool lower_body_map(y_lower *L, y_node *n, const char *rule_key, pb_value *out) {
    y_pair *when = NULL;
    y_pair *body = NULL;
    size_t bodies = 0;
    for (size_t i = 0; i < n->pair_len; i += 1) {
        y_pair *p = &n->pairs[i];
        if (slice_eq_lit(p->key, "when")) {
            when = p;
        } else if (slice_eq_lit(p->key, "thread") || slice_eq_lit(p->key, "do") || slice_eq_lit(p->key, "form")) {
            body = p;
            bodies += 1;
        } else if (slice_eq_lit(p->key, "body")) {
            return lower_fail(L, p->value->offset, "the body key was removed; use form");
        } else if (rule_key != NULL && (slice_eq_lit(p->key, rule_key) || slice_eq_lit(p->key, "reentrant"))) {
            continue;
        } else {
            return lower_fail(L, p->value->offset, "unknown key \"%.*s\" in a rule body (expected when, thread, do or form)", (int)p->key.len, p->key.ptr);
        }
    }
    if (when != NULL && when->value->kind == Y_MAP) {
        return lower_fail(L, when->value->offset, "the when: {test, then} map form was removed; write `when: EXPR` beside one body key (thread, do or form)");
    }
    if (bodies != 1) {
        return lower_fail(L, n->offset, "expected exactly one of thread, do or form (found %zu)", bodies);
    }
    pb_value inner = {0};
    bool ok;
    if (slice_eq_lit(body->key, "thread")) {
        ok = lower_thread(L, body->value, &inner);
    } else if (slice_eq_lit(body->key, "do")) {
        ok = lower_do(L, body->value, &inner);
    } else {
        ok = lower_expr(L, body->value, &inner);
    }
    if (!ok) {
        return false;
    }
    if (when == NULL) {
        *out = inner;
        return true;
    }
    pb_build_vec v = {0};
    pb_value head = {0};
    pb_value test = {0};
    if (!arena_text_lit(L, PB_SYMBOL, "when", &head) || !vec_push(L, &v, head) ||
        !lower_expr(L, when->value, &test) || !vec_push(L, &v, test) || !vec_push(L, &v, inner)) {
        return vec_done(&v, false);
    }
    ok = freeze_pb_vec(L, &v, PB_LIST, out);
    return vec_done(&v, ok);
}

static bool lower_env_config_value(y_lower *L, y_node *n, pb_value *out) {
    y_pair *p = map_find(n, "env");
    if (n->pair_len != 1 || p == NULL || p->value->kind != Y_SCALAR || p->value->scalar_kind != Y_TEXT || p->value->text.len == 0) {
        return lower_fail(L, n->offset, "env must be a single `env: NAME` entry");
    }
    pb_build_vec form = {0};
    pb_value head = {0};
    pb_value name = {0};
    if (!arena_text_lit(L, PB_SYMBOL, "env", &head) ||
        !arena_text(L, PB_STRING, p->value->text, &name) ||
        !vec_push(L, &form, head) ||
        !vec_push(L, &form, name)) {
        return vec_done(&form, false);
    }
    const bool ok = freeze_pb_vec(L, &form, PB_LIST, out);
    return vec_done(&form, ok);
}

static bool lower_config_value(y_lower *L, y_node *n, pb_value *out) {
    if (n->kind == Y_MAP) {
        if (n->pair_len == 1 && map_find(n, "env") != NULL) {
            return lower_env_config_value(L, n, out);
        }
        pb_build_vec v = {0};
        for (size_t i = 0; i < n->pair_len; i += 1) {
            pb_value key = {0};
            pb_value value = {0};
            if (!arena_text(L, PB_KEYWORD, n->pairs[i].key, &key) ||
                !lower_config_value(L, n->pairs[i].value, &value) ||
                !vec_push(L, &v, key) ||
                !vec_push(L, &v, value)) {
                return vec_done(&v, false);
            }
        }
        const bool ok = freeze_pb_vec(L, &v, PB_VECTOR, out);
        return vec_done(&v, ok);
    }
    if (n->kind == Y_LIST) {
        pb_build_vec v = {0};
        for (size_t i = 0; i < n->len; i += 1) {
            pb_value item = {0};
            if (!lower_config_value(L, n->items[i], &item) || !vec_push(L, &v, item)) {
                return vec_done(&v, false);
            }
        }
        const bool ok = freeze_pb_vec(L, &v, PB_VECTOR, out);
        return vec_done(&v, ok);
    }
    return lower_config_scalar(L, n, out);
}

static bool lower_lvc(y_lower *L, y_node *n, pb_value *out) {
    pb_build_vec form = {0};
    pb_value head = {0};
    pb_value arg = {0};
    if (!arena_text_lit(L, PB_SYMBOL, "lvc", &head) || !vec_push(L, &form, head) ||
        !lower_config_value(L, n, &arg) || !vec_push(L, &form, arg)) {
        return vec_done(&form, false);
    }
    const bool ok = freeze_pb_vec(L, &form, PB_LIST, out);
    return vec_done(&form, ok);
}

static bool lower_config_form(y_lower *L, const char *name, y_node *n, pb_value *out) {
    if (n->kind != Y_MAP) {
        return lower_fail(L, n->offset, "%s must be a map", name);
    }
    pb_build_vec form = {0};
    pb_value head = {0};
    if (!arena_text_lit(L, PB_SYMBOL, name, &head) || !vec_push(L, &form, head)) {
        return vec_done(&form, false);
    }
    for (size_t i = 0; i < n->pair_len; i += 1) {
        pb_value key = {0};
        pb_value value = {0};
        if (!arena_text(L, PB_KEYWORD, n->pairs[i].key, &key) ||
            !lower_config_value(L, n->pairs[i].value, &value) ||
            !vec_push(L, &form, key) ||
            !vec_push(L, &form, value)) {
            return vec_done(&form, false);
        }
    }
    const bool ok = freeze_pb_vec(L, &form, PB_LIST, out);
    return vec_done(&form, ok);
}

// Lowers one `on` item to (on "SUB" [:reentrant B] BODY), or, with every set, one
// `every` item to (every :ms N [:reentrant B] BODY).
static bool lower_rule(y_lower *L, y_node *n, bool every, pb_value *out) {
    if (n->kind != Y_MAP) {
        return lower_fail(L, n->offset, "each %s item must be a map", every ? "every" : "on");
    }
    pb_build_vec form = {0};
    pb_value head = {0};
    if (!arena_text_lit(L, PB_SYMBOL, every ? "every" : "on", &head) || !vec_push(L, &form, head)) {
        return vec_done(&form, false);
    }
    if (every) {
        y_pair *ms = map_find(n, "ms");
        pb_value key = {0};
        if (ms == NULL || ms->value->kind != Y_SCALAR || ms->value->scalar_kind != Y_NUMBER) {
            vec_done(&form, false);
            return lower_fail(L, ms != NULL ? ms->value->offset : n->offset, "every item needs `ms: NUMBER`");
        }
        if (!arena_text_lit(L, PB_KEYWORD, "ms", &key) || !vec_push(L, &form, key) ||
            !vec_push(L, &form, (pb_value){.kind = PB_NUMBER, .number = ms->value->number})) {
            return vec_done(&form, false);
        }
    } else {
        y_pair *sub = map_find(n, "sub");
        pb_value filter = {0};
        if (sub == NULL || sub->value->kind != Y_SCALAR || sub->value->scalar_kind != Y_TEXT) {
            vec_done(&form, false);
            return lower_fail(L, sub != NULL ? sub->value->offset : n->offset, "on item needs `sub: \"SUBJECT\"`");
        }
        if (!arena_text(L, PB_STRING, sub->value->text, &filter) || !vec_push(L, &form, filter)) {
            return vec_done(&form, false);
        }
    }

    y_pair *reentrant = map_find(n, "reentrant");
    if (reentrant != NULL) {
        if (reentrant->value->kind != Y_SCALAR || reentrant->value->scalar_kind != Y_BOOL) {
            vec_done(&form, false);
            return lower_fail(L, reentrant->value->offset, "reentrant must be true or false");
        }
        pb_value key = {0};
        pb_value value = {.kind = PB_BOOL, .boolean = reentrant->value->boolean};
        if (!arena_text_lit(L, PB_KEYWORD, "reentrant", &key) ||
            !vec_push(L, &form, key) ||
            !vec_push(L, &form, value)) {
            return vec_done(&form, false);
        }
    }

    pb_value body = {0};
    if (!lower_body_map(L, n, every ? "ms" : "sub", &body) || !vec_push(L, &form, body)) {
        return vec_done(&form, false);
    }
    const bool ok = freeze_pb_vec(L, &form, PB_LIST, out);
    return vec_done(&form, ok);
}

static bool lower_rules(y_lower *L, y_node *n, bool every, pb_build_vec *forms) {
    if (n->kind != Y_LIST) {
        return lower_fail(L, n->offset, "%s must be a list of rule maps", every ? "every" : "on");
    }
    for (size_t i = 0; i < n->len; i += 1) {
        pb_value form = {0};
        if (!lower_rule(L, n->items[i], every, &form) || !vec_push(L, forms, form)) {
            return false;
        }
    }
    return true;
}

static bool lower_root(y_lower *L, y_node *root, pb_values *out) {
    if (root == NULL || root->kind != Y_MAP) {
        return lower_fail(L, root != NULL ? root->offset : 0, "the root must be a map with lvc, on, every, export or import");
    }
    pb_build_vec forms = {0};
    for (size_t i = 0; i < root->pair_len; i += 1) {
        y_pair *p = &root->pairs[i];
        if (slice_eq_lit(p->key, "on") || slice_eq_lit(p->key, "every")) {
            if (!lower_rules(L, p->value, slice_eq_lit(p->key, "every"), &forms)) {
                return vec_done(&forms, false);
            }
        } else if (slice_eq_lit(p->key, "lvc")) {
            pb_value form = {0};
            if (!lower_lvc(L, p->value, &form) || !vec_push(L, &forms, form)) {
                return vec_done(&forms, false);
            }
        } else if (slice_eq_lit(p->key, "export") || slice_eq_lit(p->key, "bridge") || slice_eq_lit(p->key, "import")) {
            char name[16];
            memcpy(name, p->key.ptr, p->key.len);
            name[p->key.len] = '\0';
            pb_value form = {0};
            if (!lower_config_form(L, name, p->value, &form) || !vec_push(L, &forms, form)) {
                return vec_done(&forms, false);
            }
        } else {
            vec_done(&forms, false);
            return lower_fail(L, p->value->offset, "unknown top-level key \"%.*s\" (expected lvc, on, every, export or import)", (int)p->key.len, p->key.ptr);
        }
    }
    pb_value *items = pb_arena_alloc(L->arena, forms.len * sizeof items[0], alignof(pb_value));
    if (items == NULL && forms.len != 0) {
        vec_done(&forms, false);
        return lower_oom(L, 0);
    }
    if (forms.len != 0) {
        memcpy(items, forms.items, forms.len * sizeof items[0]);
    }
    *out = (pb_values){.items = items, .len = forms.len};
    return vec_done(&forms, true);
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
    y_lower lower = {.arena = arena, .err = PB_PARSE_OK};
    if (p.err == PB_PARSE_OK && !lower_root(&lower, root, &forms)) {
        p.err = lower.err != PB_PARSE_OK ? lower.err : PB_PARSE_INVALID_YAML;
        p.err_offset = lower.err != PB_PARSE_OK ? lower.err_offset : (root != NULL ? root->offset : 0);
    }
    node_free(root);
    free(p.lines);
    if (p.err != PB_PARSE_OK) {
        pb_parse_result r = {.err = p.err, .err_offset = p.err_offset};
        memcpy(r.err_msg, lower.msg, sizeof r.err_msg);
        return r;
    }
    return (pb_parse_result){.err = PB_PARSE_OK, .forms = forms};
}
