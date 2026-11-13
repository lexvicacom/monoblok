#include "pb_regex.h"
#include "pb_regex_internal.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

#define RX_INF 0xFFFFu    // repeat max meaning unbounded
#define RX_NONE 0xFFFFu   // end of a patch chain
#define RX_UNSET SIZE_MAX // capture slot not written

typedef enum pb_rx_kind {
    N_EMPTY,
    N_CHAR,
    N_SET,
    N_BOL,
    N_EOL,
    N_WORDB,
    N_NWORDB,
    N_CAT,
    N_ALT,
    N_GROUP,
    N_REPEAT,
} pb_rx_kind;

// Parse tree node in the compile scratch arena. CAT/ALT chain children through
// `kid` and `next`; GROUP/REPEAT use `kid` alone.
typedef struct pb_rx_node {
    uint8_t kind;
    bool greedy;
    uint8_t byte;       // N_CHAR
    uint8_t group;      // N_GROUP capture index, 1-based
    uint16_t min;       // N_REPEAT
    uint16_t max;       // N_REPEAT, RX_INF when unbounded
    uint16_t off;       // pattern offset, for error reports
    const uint8_t *set; // N_SET bitmap, in the scratch arena
    const uint8_t *out_set; // regex-arena copy of `set`, made on first emit
    struct pb_rx_node *kid;
    struct pb_rx_node *next;
} pb_rx_node;

// Compile state shared by parser and emitter, on the caller's stack. The first
// error wins; later steps unwind with NULL/false.
typedef struct pb_rx_ctx {
    pb_arena *arena;   // caller's arena: receives only the finished regex
    pb_arena *scratch; // parse tree and class bitmaps, freed before return
    const char *src;   // borrowed pattern bytes
    size_t len;
    size_t pos;
    bool icase;
    bool dotall;
    unsigned ngroups;
    pb_rx_inst *prog; // stack buffer of PB_REGEX_MAX_PROGRAM entries
    size_t pc;
    pb_regex_error err;
    size_t err_off;
    const char *err_msg;
} pb_rx_ctx;

// One Pike VM thread list: pcs of consuming/MATCH instructions in priority
// order, with `nc` capture slots per entry stored flat in caps.
typedef struct pb_rx_list {
    uint16_t *pc;
    size_t *caps;
    size_t n;
} pb_rx_list;

// Explicit epsilon-closure stack entry: explore `pc`, or (slot >= 0) restore a capture.
typedef struct pb_rx_frame {
    size_t old;
    uint16_t pc;
    int16_t slot;
} pb_rx_frame;

// Compiled regex, arena-owned. Match scratch is mutated per match: not reentrant.
struct pb_regex {
    pb_slice source;
    pb_rx_inst *prog;
    size_t nprog;
    size_t ngroups;
    size_t ncap;       // 2 * (ngroups + 1)
    bool anchored;     // every match must start at offset 0
    int first_byte;    // byte every match must start with, or -1
    bool use_first_set; // first_ok is valid: pattern cannot match empty and is not anchored
    uint8_t first_ok[256]; // bytes a match can start with (superset)
    size_t max_len;    // longest possible match in bytes, SIZE_MAX when unbounded
    bool use_window;   // DFA end offset may bound the VM start (see pb_regex_search)
    pb_rx_dfa *dfa;    // capture-free fast path, NULL when absent or disabled
    uint32_t gen;      // visited generation, bumped per text position (see vm_run)
    uint32_t *visited; // per-pc generation marks
    pb_rx_frame *stack;
    pb_rx_list lists[2];
    size_t *cur;       // captures of the thread being expanded
    size_t *best;      // captures of the best match so far
};

// ---------- errors ----------

static void *rx_fail(pb_rx_ctx *c, pb_regex_error err, size_t off, const char *msg) {
    if (c->err == PB_REGEX_OK) {
        c->err = err;
        c->err_off = off;
        c->err_msg = msg;
    }
    return NULL;
}

const char *pb_regex_error_name(pb_regex_error err) {
    switch (err) {
    case PB_REGEX_OK: return "ok";
    case PB_REGEX_OOM: return "out of memory";
    case PB_REGEX_TOO_LONG: return "pattern too long";
    case PB_REGEX_TOO_LARGE: return "pattern too large";
    case PB_REGEX_TOO_MANY_GROUPS: return "too many groups";
    case PB_REGEX_TOO_DEEP: return "nested too deeply";
    case PB_REGEX_SYNTAX: return "syntax error";
    case PB_REGEX_UNSUPPORTED: return "unsupported syntax";
    }
    return "unknown error";
}

// ---------- byte sets ----------

static void set_add(uint8_t *set, unsigned c) { set[c >> 3] = (uint8_t)(set[c >> 3] | (1u << (c & 7))); }
static void set_remove(uint8_t *set, unsigned c) { set[c >> 3] = (uint8_t)(set[c >> 3] & ~(1u << (c & 7))); }

static size_t set_count(const uint8_t *set) {
    size_t n = 0;
    for (unsigned c = 0; c < 256; c += 1) {
        n += pb_rx_set_has(set, c) ? 1 : 0;
    }
    return n;
}

static void set_negate(uint8_t *set) {
    for (size_t i = 0; i < RX_SET_BYTES; i += 1) {
        set[i] = (uint8_t)~set[i];
    }
}

static void set_fold_ascii_case(uint8_t *set) {
    for (unsigned c = 'a'; c <= 'z'; c += 1) {
        if (pb_rx_set_has(set, c) || pb_rx_set_has(set, c - 32)) {
            set_add(set, c);
            set_add(set, c - 32);
        }
    }
}

static bool is_ascii_alnum(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_ascii_letter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void set_add_builtin(uint8_t *set, char cls) {
    uint8_t tmp[RX_SET_BYTES] = {0};
    switch (cls) {
    case 'd': case 'D':
        for (unsigned c = '0'; c <= '9'; c += 1) set_add(tmp, c);
        break;
    case 'w': case 'W':
        for (unsigned c = 0; c < 256; c += 1) {
            if (pb_rx_is_word_byte((unsigned char)c)) set_add(tmp, c);
        }
        break;
    default: // 's', 'S': space, \t \n \v \f \r
        set_add(tmp, ' ');
        for (unsigned c = 9; c <= 13; c += 1) set_add(tmp, c);
        break;
    }
    if (cls == 'D' || cls == 'W' || cls == 'S') {
        set_negate(tmp);
    }
    for (size_t i = 0; i < RX_SET_BYTES; i += 1) {
        set[i] = (uint8_t)(set[i] | tmp[i]);
    }
}

// ---------- parser ----------

static pb_rx_node *node_new(pb_rx_ctx *c, pb_rx_kind kind, size_t off) {
    pb_rx_node *n = pb_arena_alloc(c->scratch, sizeof *n, alignof(pb_rx_node));
    if (n == NULL) {
        return rx_fail(c, PB_REGEX_OOM, off, "out of memory");
    }
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    n->greedy = true;
    n->off = (uint16_t)off;
    return n;
}

static uint8_t *set_new(pb_rx_ctx *c, size_t off) {
    uint8_t *set = pb_arena_alloc(c->scratch, RX_SET_BYTES, 1);
    if (set == NULL) {
        return rx_fail(c, PB_REGEX_OOM, off, "out of memory");
    }
    memset(set, 0, RX_SET_BYTES);
    return set;
}

// Turns a finished bitmap into a node; a single-byte set becomes a plain literal.
static pb_rx_node *node_from_set(pb_rx_ctx *c, uint8_t *set, size_t off) {
    size_t count = set_count(set);
    if (count == 0) {
        return rx_fail(c, PB_REGEX_SYNTAX, off, "empty character class");
    }
    if (count == 1) {
        pb_rx_node *n = node_new(c, N_CHAR, off);
        if (n == NULL) return NULL;
        for (unsigned b = 0; b < 256; b += 1) {
            if (pb_rx_set_has(set, b)) n->byte = (uint8_t)b;
        }
        return n;
    }
    pb_rx_node *n = node_new(c, N_SET, off);
    if (n != NULL) n->set = set;
    return n;
}

static pb_rx_node *node_literal(pb_rx_ctx *c, uint8_t byte, size_t off) {
    if (c->icase && is_ascii_letter(byte)) {
        uint8_t *set = set_new(c, off);
        if (set == NULL) return NULL;
        set_add(set, byte);
        set_fold_ascii_case(set);
        return node_from_set(c, set, off);
    }
    pb_rx_node *n = node_new(c, N_CHAR, off);
    if (n != NULL) n->byte = byte;
    return n;
}

static int hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

typedef enum pb_rx_escape {
    ESC_ERROR,
    ESC_BYTE,   // *byte holds one literal byte
    ESC_CLASS,  // *cls is one of d D w W s S
    ESC_ASSERT, // *cls is b or B (outside classes only)
} pb_rx_escape;

static pb_rx_escape parse_escape(pb_rx_ctx *c, bool in_class, uint8_t *byte, char *cls) {
    size_t off = c->pos;
    if (off + 1 >= c->len) {
        rx_fail(c, PB_REGEX_SYNTAX, off, "trailing backslash");
        return ESC_ERROR;
    }
    unsigned char e = (unsigned char)c->src[off + 1];
    c->pos += 2;
    switch (e) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
        *cls = (char)e;
        return ESC_CLASS;
    case 'b': case 'B':
        if (in_class) {
            rx_fail(c, PB_REGEX_UNSUPPORTED, off, "\\b inside a character class is not supported");
            return ESC_ERROR;
        }
        *cls = (char)e;
        return ESC_ASSERT;
    case 'n': *byte = '\n'; return ESC_BYTE;
    case 'r': *byte = '\r'; return ESC_BYTE;
    case 't': *byte = '\t'; return ESC_BYTE;
    case 'x': {
        int hi = c->pos < c->len ? hex_value((unsigned char)c->src[c->pos]) : -1;
        int lo = c->pos + 1 < c->len ? hex_value((unsigned char)c->src[c->pos + 1]) : -1;
        if (hi < 0 || lo < 0) {
            rx_fail(c, PB_REGEX_SYNTAX, off, "\\x needs two hex digits");
            return ESC_ERROR;
        }
        c->pos += 2;
        *byte = (uint8_t)(hi * 16 + lo);
        return ESC_BYTE;
    }
    default:
        break;
    }
    if (e >= '1' && e <= '9') {
        rx_fail(c, PB_REGEX_UNSUPPORTED, off, "backreferences are not supported");
    } else if (e == 'p' || e == 'P') {
        rx_fail(c, PB_REGEX_UNSUPPORTED, off, "\\p classes are not supported");
    } else if (is_ascii_alnum(e)) {
        rx_fail(c, PB_REGEX_UNSUPPORTED, off, "unknown escape");
    } else {
        *byte = e; // escaped punctuation, space, NUL or high byte is literal
        return ESC_BYTE;
    }
    return ESC_ERROR;
}

// Parses one class member. Returns 1 for a single byte in *lo, 2 when a
// builtin class was added directly to set, 0 on error.
static int parse_class_item(pb_rx_ctx *c, uint8_t *set, unsigned *lo) {
    if (c->src[c->pos] != '\\') {
        *lo = (unsigned char)c->src[c->pos];
        c->pos += 1;
        return 1;
    }
    uint8_t byte = 0;
    char cls = 0;
    switch (parse_escape(c, true, &byte, &cls)) {
    case ESC_BYTE:
        *lo = byte;
        return 1;
    case ESC_CLASS:
        set_add_builtin(set, cls);
        return 2;
    default:
        return 0;
    }
}

static pb_rx_node *parse_class(pb_rx_ctx *c) {
    size_t off = c->pos;
    c->pos += 1;
    bool negate = false;
    if (c->pos < c->len && c->src[c->pos] == '^') {
        negate = true;
        c->pos += 1;
    }
    uint8_t *set = set_new(c, off);
    if (set == NULL) return NULL;
    bool first = true;
    for (;;) {
        if (c->pos >= c->len) {
            return rx_fail(c, PB_REGEX_SYNTAX, off, "unterminated character class");
        }
        unsigned char ch = (unsigned char)c->src[c->pos];
        if (ch == ']' && !first) {
            c->pos += 1;
            break;
        }
        first = false;
        if (ch == '[' && c->pos + 1 < c->len) {
            char next = c->src[c->pos + 1];
            if (next == ':' || next == '.' || next == '=') {
                return rx_fail(c, PB_REGEX_UNSUPPORTED, c->pos, "POSIX character classes are not supported");
            }
        }
        unsigned lo = 0;
        int kind = parse_class_item(c, set, &lo);
        if (kind == 0) return NULL;
        if (kind == 2) continue;
        if (c->pos + 1 < c->len && c->src[c->pos] == '-' && c->src[c->pos + 1] != ']') {
            c->pos += 1;
            size_t hi_off = c->pos;
            unsigned hi = 0;
            kind = parse_class_item(c, set, &hi);
            if (kind == 0) return NULL;
            if (kind == 2) {
                return rx_fail(c, PB_REGEX_SYNTAX, hi_off, "invalid range endpoint");
            }
            if (hi < lo) {
                return rx_fail(c, PB_REGEX_SYNTAX, hi_off, "reversed range in character class");
            }
            for (unsigned b = lo; b <= hi; b += 1) set_add(set, b);
        } else {
            set_add(set, lo);
        }
    }
    if (c->icase) set_fold_ascii_case(set);
    if (negate) set_negate(set);
    return node_from_set(c, set, off);
}

static pb_rx_node *parse_alt(pb_rx_ctx *c, unsigned depth);

static pb_rx_node *parse_group(pb_rx_ctx *c, unsigned depth) {
    size_t off = c->pos;
    unsigned group = 0;
    c->pos += 1;
    if (c->pos < c->len && c->src[c->pos] == '?') {
        if (c->pos + 1 >= c->len) {
            return rx_fail(c, PB_REGEX_SYNTAX, off, "unterminated group");
        }
        char k = c->src[c->pos + 1];
        if (k == ':') {
            c->pos += 2;
        } else if (k == '=' || k == '!') {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, "lookahead is not supported");
        } else if (k == '<') {
            bool look = c->pos + 2 < c->len && (c->src[c->pos + 2] == '=' || c->src[c->pos + 2] == '!');
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, look ? "lookbehind is not supported" : "named groups are not supported");
        } else if (k == 'P' || k == '\'') {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, "named groups are not supported");
        } else if (k == '>') {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, "atomic groups are not supported");
        } else if (is_ascii_alnum((unsigned char)k) || k == '-') {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, "inline flags are only allowed at the start of the pattern");
        } else {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, off, "unknown group syntax");
        }
    } else {
        if (c->ngroups >= PB_REGEX_MAX_GROUPS) {
            return rx_fail(c, PB_REGEX_TOO_MANY_GROUPS, off, "too many capture groups");
        }
        c->ngroups += 1;
        group = c->ngroups;
    }
    pb_rx_node *inner = parse_alt(c, depth + 1);
    if (inner == NULL) return NULL;
    if (c->pos >= c->len || c->src[c->pos] != ')') {
        return rx_fail(c, PB_REGEX_SYNTAX, off, "unterminated group");
    }
    c->pos += 1;
    if (group == 0) return inner;
    pb_rx_node *g = node_new(c, N_GROUP, off);
    if (g == NULL) return NULL;
    g->group = (uint8_t)group;
    g->kid = inner;
    return g;
}

static pb_rx_node *parse_atom(pb_rx_ctx *c, unsigned depth) {
    size_t off = c->pos;
    unsigned char ch = (unsigned char)c->src[off];
    switch (ch) {
    case '(':
        return parse_group(c, depth);
    case '[':
        return parse_class(c);
    case '.': {
        uint8_t *set = set_new(c, off);
        if (set == NULL) return NULL;
        memset(set, 0xFF, RX_SET_BYTES);
        if (!c->dotall) set_remove(set, '\n');
        c->pos += 1;
        return node_from_set(c, set, off);
    }
    case '^':
        c->pos += 1;
        return node_new(c, N_BOL, off);
    case '$':
        c->pos += 1;
        return node_new(c, N_EOL, off);
    case '*': case '+': case '?':
        return rx_fail(c, PB_REGEX_SYNTAX, off, "quantifier has nothing to repeat");
    case '{':
        return rx_fail(c, PB_REGEX_SYNTAX, off, "'{' starts a repetition here; write \\{ for a literal brace");
    case '\\': {
        uint8_t byte = 0;
        char cls = 0;
        switch (parse_escape(c, false, &byte, &cls)) {
        case ESC_BYTE:
            return node_literal(c, byte, off);
        case ESC_CLASS: {
            uint8_t *set = set_new(c, off);
            if (set == NULL) return NULL;
            set_add_builtin(set, cls);
            return node_from_set(c, set, off);
        }
        case ESC_ASSERT:
            return node_new(c, cls == 'b' ? N_WORDB : N_NWORDB, off);
        default:
            return NULL;
        }
    }
    default:
        c->pos += 1;
        return node_literal(c, ch, off);
    }
}

// Reads a decimal repeat bound at c->pos; returns -1 when there is no digit, -2 on error.
static int parse_bound(pb_rx_ctx *c) {
    int v = -1;
    while (c->pos < c->len && c->src[c->pos] >= '0' && c->src[c->pos] <= '9') {
        v = (v < 0 ? 0 : v) * 10 + (c->src[c->pos] - '0');
        if (v > PB_REGEX_MAX_REPEAT) {
            rx_fail(c, PB_REGEX_TOO_LARGE, c->pos, "repetition bound exceeds 255");
            return -2;
        }
        c->pos += 1;
    }
    return v;
}

// True for ^ $ \b \B, also when wrapped in groups: (?:\b)? and (\b)? are both rejected.
static bool is_assertion(const pb_rx_node *n) {
    if (n->kind == N_GROUP) return is_assertion(n->kid);
    return n->kind == N_BOL || n->kind == N_EOL || n->kind == N_WORDB || n->kind == N_NWORDB;
}

static pb_rx_node *parse_quantifier(pb_rx_ctx *c, pb_rx_node *atom) {
    if (c->pos >= c->len) return atom;
    size_t qoff = c->pos;
    unsigned min = 0, max = 0;
    switch (c->src[qoff]) {
    case '*': min = 0; max = RX_INF; c->pos += 1; break;
    case '+': min = 1; max = RX_INF; c->pos += 1; break;
    case '?': min = 0; max = 1; c->pos += 1; break;
    case '{': {
        c->pos += 1;
        int lo = parse_bound(c);
        if (lo == -2) return NULL;
        if (lo < 0) return rx_fail(c, PB_REGEX_SYNTAX, qoff, "invalid repetition, expected {n}, {n,} or {n,m}; write \\{ for a literal brace");
        min = (unsigned)lo;
        max = min;
        if (c->pos < c->len && c->src[c->pos] == ',') {
            c->pos += 1;
            int hi = parse_bound(c);
            if (hi == -2) return NULL;
            max = hi < 0 ? RX_INF : (unsigned)hi;
        }
        if (c->pos >= c->len || c->src[c->pos] != '}') {
            return rx_fail(c, PB_REGEX_SYNTAX, qoff, "invalid repetition, expected {n}, {n,} or {n,m}; write \\{ for a literal brace");
        }
        c->pos += 1;
        if (max != RX_INF && max < min) {
            return rx_fail(c, PB_REGEX_SYNTAX, qoff, "reversed repetition bounds");
        }
        break;
    }
    default:
        return atom;
    }
    if (is_assertion(atom)) {
        return rx_fail(c, PB_REGEX_SYNTAX, qoff, "quantifier on an assertion");
    }
    bool greedy = true;
    if (c->pos < c->len && c->src[c->pos] == '?') {
        greedy = false;
        c->pos += 1;
    }
    if (c->pos < c->len) {
        char next = c->src[c->pos];
        if (next == '+' && greedy) {
            return rx_fail(c, PB_REGEX_UNSUPPORTED, c->pos, "possessive quantifiers are not supported");
        }
        if (next == '*' || next == '+' || next == '?' || next == '{') {
            return rx_fail(c, PB_REGEX_SYNTAX, c->pos, "nested quantifier");
        }
    }
    pb_rx_node *rep = node_new(c, N_REPEAT, qoff);
    if (rep == NULL) return NULL;
    rep->kid = atom;
    rep->min = (uint16_t)min;
    rep->max = (uint16_t)max;
    rep->greedy = greedy;
    return rep;
}

static pb_rx_node *parse_cat(pb_rx_ctx *c, unsigned depth) {
    size_t start = c->pos;
    pb_rx_node *head = NULL, *tail = NULL;
    size_t count = 0;
    while (c->pos < c->len && c->src[c->pos] != '|' && c->src[c->pos] != ')') {
        pb_rx_node *atom = parse_atom(c, depth);
        if (atom == NULL) return NULL;
        atom = parse_quantifier(c, atom);
        if (atom == NULL) return NULL;
        if (tail == NULL) head = atom; else tail->next = atom;
        tail = atom;
        count += 1;
    }
    if (count == 0) return node_new(c, N_EMPTY, start);
    if (count == 1) return head;
    pb_rx_node *cat = node_new(c, N_CAT, start);
    if (cat != NULL) cat->kid = head;
    return cat;
}

static pb_rx_node *parse_alt(pb_rx_ctx *c, unsigned depth) {
    if (depth > PB_REGEX_MAX_DEPTH) {
        return rx_fail(c, PB_REGEX_TOO_DEEP, c->pos, "groups nested too deeply");
    }
    size_t start = c->pos;
    pb_rx_node *head = NULL, *tail = NULL;
    size_t count = 0;
    for (;;) {
        pb_rx_node *branch = parse_cat(c, depth);
        if (branch == NULL) return NULL;
        if (tail == NULL) head = branch; else tail->next = branch;
        tail = branch;
        count += 1;
        if (c->pos < c->len && c->src[c->pos] == '|') {
            c->pos += 1;
            continue;
        }
        break;
    }
    if (count == 1) return head;
    pb_rx_node *alt = node_new(c, N_ALT, start);
    if (alt != NULL) alt->kid = head;
    return alt;
}

// Consumes a leading (?i) / (?s) / (?is) flag group, if present.
static void parse_flag_prefix(pb_rx_ctx *c) {
    if (c->len < 4 || c->src[0] != '(' || c->src[1] != '?') return;
    size_t i = 2;
    bool icase = false, dotall = false;
    while (i < c->len && (c->src[i] == 'i' || c->src[i] == 's')) {
        if (c->src[i] == 'i') icase = true; else dotall = true;
        i += 1;
    }
    if (i == 2 || i >= c->len || c->src[i] != ')') return;
    c->icase = icase;
    c->dotall = dotall;
    c->pos = i + 1;
}

// ---------- analysis ----------

static bool starts_anchored(const pb_rx_node *n) {
    switch (n->kind) {
    case N_BOL: return true;
    case N_CAT: return starts_anchored(n->kid);
    case N_GROUP: return starts_anchored(n->kid);
    case N_REPEAT: return n->min >= 1 && starts_anchored(n->kid);
    case N_ALT:
        for (const pb_rx_node *k = n->kid; k != NULL; k = k->next) {
            if (!starts_anchored(k)) return false;
        }
        return true;
    default: return false;
    }
}

// Byte every match must begin with, or -1. Word-boundary assertions are transparent.
static int required_first_byte(const pb_rx_node *n) {
    switch (n->kind) {
    case N_CHAR: return n->byte;
    case N_GROUP: return required_first_byte(n->kid);
    case N_REPEAT: return n->min >= 1 ? required_first_byte(n->kid) : -1;
    case N_CAT:
        for (const pb_rx_node *k = n->kid; k != NULL; k = k->next) {
            if (k->kind == N_WORDB || k->kind == N_NWORDB) continue;
            return required_first_byte(k);
        }
        return -1;
    case N_ALT: {
        int first = required_first_byte(n->kid);
        for (const pb_rx_node *k = n->kid->next; first >= 0 && k != NULL; k = k->next) {
            if (required_first_byte(k) != first) first = -1;
        }
        return first;
    }
    default: return -1;
    }
}

// Longest possible match length in bytes, SIZE_MAX when unbounded or too big to count.
static size_t max_match_len(const pb_rx_node *n) {
    switch (n->kind) {
    case N_CHAR:
    case N_SET: return 1;
    case N_CAT: {
        size_t sum = 0;
        for (const pb_rx_node *k = n->kid; k != NULL; k = k->next) {
            size_t v = max_match_len(k);
            if (v == SIZE_MAX || sum > SIZE_MAX - v) return SIZE_MAX;
            sum += v;
        }
        return sum;
    }
    case N_ALT: {
        size_t best = 0;
        for (const pb_rx_node *k = n->kid; k != NULL; k = k->next) {
            size_t v = max_match_len(k);
            if (v > best) best = v;
        }
        return best;
    }
    case N_GROUP: return max_match_len(n->kid);
    case N_REPEAT: {
        size_t v = max_match_len(n->kid);
        if (v == 0 || n->max == 0) return 0;
        if (n->max == RX_INF || v == SIZE_MAX || v > SIZE_MAX / n->max) return SIZE_MAX;
        return v * n->max;
    }
    default: return 0; // empty and assertions
    }
}

// Fills re->first_ok from pc 0 over epsilon edges. Assertions count as true
// (a superset). Unused when MATCH is reachable or the pattern is anchored.
static void compute_first_set(pb_regex *re) {
    uint8_t seen[PB_REGEX_MAX_PROGRAM] = {0};
    uint16_t todo[2 * PB_REGEX_MAX_PROGRAM + 2];
    size_t n = 0;
    memset(re->first_ok, 0, sizeof re->first_ok);
    re->use_first_set = false;
    todo[n++] = 0;
    while (n > 0) {
        unsigned pc = todo[--n];
        if (seen[pc]) continue;
        seen[pc] = 1;
        const pb_rx_inst *in = &re->prog[pc];
        switch (in->op) {
        case OP_JMP: todo[n++] = in->x; break;
        case OP_SPLIT:
            todo[n++] = in->y;
            todo[n++] = in->x;
            break;
        case OP_MATCH: return;
        case OP_CHAR: re->first_ok[in->byte] = 1; break;
        case OP_SET:
            for (unsigned c = 0; c < 256; c += 1) {
                if (pb_rx_set_has(in->set, c)) re->first_ok[c] = 1;
            }
            break;
        default: todo[n++] = (uint16_t)(pc + 1); break; // SAVE and assertions
        }
    }
    re->use_first_set = !re->anchored;
}

// ---------- emitter ----------

// Appends one instruction and returns its index, or -1 once the program cap is hit.
static int emit_inst(pb_rx_ctx *c, pb_rx_op op, uint8_t byte, unsigned x, unsigned y, const uint8_t *set, size_t off) {
    if (c->pc >= PB_REGEX_MAX_PROGRAM) {
        rx_fail(c, PB_REGEX_TOO_LARGE, off, "compiled program too large");
        return -1;
    }
    pb_rx_inst *in = &c->prog[c->pc];
    in->op = (uint8_t)op;
    in->byte = byte;
    in->x = (uint16_t)x;
    in->y = (uint16_t)y;
    in->set = set;
    c->pc += 1;
    return (int)(c->pc - 1);
}

static bool emit_node(pb_rx_ctx *c, pb_rx_node *n);

static bool emit_repeat(pb_rx_ctx *c, pb_rx_node *n) {
    pb_rx_node *sub = n->kid;
    unsigned min = n->min, max = n->max;
    bool unbounded = max == RX_INF;
    // A trailing "+" shares its last mandatory copy with the loop.
    unsigned fixed = (unbounded && min > 0) ? min - 1 : min;
    for (unsigned i = 0; i < fixed; i += 1) {
        size_t before = c->pc;
        if (!emit_node(c, sub)) return false;
        if (c->pc == before) return true; // zero-width body: repeating it changes nothing
    }
    if (unbounded && min == 0) {
        // x* is emitted as (?:x+)?, so an iteration that matched empty leaves the
        // loop (Perl, Go 1.16+): (a*)* on "b" keeps its 0..0 span.
        int split = emit_inst(c, OP_SPLIT, 0, 0, 0, NULL, n->off);
        if (split < 0) return false;
        unsigned loop = (unsigned)c->pc;
        if (!emit_node(c, sub)) return false;
        unsigned after = (unsigned)c->pc + 1;
        if (emit_inst(c, OP_SPLIT, 0, n->greedy ? loop : after, n->greedy ? after : loop, NULL, n->off) < 0) return false;
        c->prog[split].x = (uint16_t)(n->greedy ? loop : after);
        c->prog[split].y = (uint16_t)(n->greedy ? after : loop);
    } else if (unbounded) {
        unsigned loop = (unsigned)c->pc;
        if (!emit_node(c, sub)) return false;
        if (c->pc == loop) return true;
        unsigned after = (unsigned)c->pc + 1;
        if (emit_inst(c, OP_SPLIT, 0, n->greedy ? loop : after, n->greedy ? after : loop, NULL, n->off) < 0) return false;
    } else {
        // (max - min) nested optionals, exits chained until the end is known.
        unsigned chain = RX_NONE;
        for (unsigned i = min; i < max; i += 1) {
            unsigned body = (unsigned)c->pc + 1;
            int split = emit_inst(c, OP_SPLIT, 0, n->greedy ? body : chain, n->greedy ? chain : body, NULL, n->off);
            if (split < 0) return false;
            chain = (unsigned)split;
            if (!emit_node(c, sub)) return false;
        }
        unsigned end = (unsigned)c->pc;
        while (chain != RX_NONE) {
            pb_rx_inst *in = &c->prog[chain];
            unsigned next = n->greedy ? in->y : in->x;
            if (n->greedy) in->y = (uint16_t)end; else in->x = (uint16_t)end;
            chain = next;
        }
    }
    return true;
}

static bool emit_alt(pb_rx_ctx *c, pb_rx_node *n) {
    unsigned jumps = RX_NONE; // chain of JMPs to patch to the end
    for (pb_rx_node *k = n->kid; k != NULL; k = k->next) {
        if (k->next == NULL) {
            if (!emit_node(c, k)) return false;
            break;
        }
        int split = emit_inst(c, OP_SPLIT, 0, 0, 0, NULL, n->off);
        if (split < 0) return false;
        c->prog[split].x = (uint16_t)(split + 1);
        if (!emit_node(c, k)) return false;
        int jmp = emit_inst(c, OP_JMP, 0, jumps, 0, NULL, n->off);
        if (jmp < 0) return false;
        jumps = (unsigned)jmp;
        c->prog[split].y = (uint16_t)c->pc;
    }
    while (jumps != RX_NONE) {
        unsigned next = c->prog[jumps].x;
        c->prog[jumps].x = (uint16_t)c->pc;
        jumps = next;
    }
    return true;
}

static bool emit_node(pb_rx_ctx *c, pb_rx_node *n) {
    switch (n->kind) {
    case N_EMPTY: return true;
    case N_CHAR: return emit_inst(c, OP_CHAR, n->byte, 0, 0, NULL, n->off) >= 0;
    case N_SET:
        if (n->out_set == NULL) {
            uint8_t *copy = pb_arena_alloc(c->arena, RX_SET_BYTES, 1);
            if (copy == NULL) {
                rx_fail(c, PB_REGEX_OOM, n->off, "out of memory");
                return false;
            }
            memcpy(copy, n->set, RX_SET_BYTES);
            n->out_set = copy;
        }
        return emit_inst(c, OP_SET, 0, 0, 0, n->out_set, n->off) >= 0;
    case N_BOL: return emit_inst(c, OP_BOL, 0, 0, 0, NULL, n->off) >= 0;
    case N_EOL: return emit_inst(c, OP_EOL, 0, 0, 0, NULL, n->off) >= 0;
    case N_WORDB: return emit_inst(c, OP_WORDB, 0, 0, 0, NULL, n->off) >= 0;
    case N_NWORDB: return emit_inst(c, OP_NWORDB, 0, 0, 0, NULL, n->off) >= 0;
    case N_CAT:
        for (pb_rx_node *k = n->kid; k != NULL; k = k->next) {
            if (!emit_node(c, k)) return false;
        }
        return true;
    case N_ALT: return emit_alt(c, n);
    case N_GROUP:
        if (emit_inst(c, OP_SAVE, 0, 2u * n->group, 0, NULL, n->off) < 0) return false;
        if (!emit_node(c, n->kid)) return false;
        return emit_inst(c, OP_SAVE, 0, 2u * n->group + 1, 0, NULL, n->off) >= 0;
    case N_REPEAT: return emit_repeat(c, n);
    }
    return false;
}

// ---------- compile ----------

static void copy_message(char *dst, size_t cap, const char *msg) {
    if (dst == NULL || cap == 0) return;
    size_t n = strlen(msg);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, msg, n);
    dst[n] = '\0';
}

static void *scratch_alloc(pb_arena *arena, size_t count, size_t size, size_t align) {
    if (size != 0 && count > SIZE_MAX / size) return NULL;
    return pb_arena_alloc(arena, count * size, align);
}

static bool regex_alloc_scratch(pb_arena *arena, pb_regex *re) {
    size_t n = re->nprog;
    re->visited = scratch_alloc(arena, n, sizeof *re->visited, alignof(uint32_t));
    re->stack = scratch_alloc(arena, n + 2, sizeof *re->stack, alignof(pb_rx_frame));
    re->cur = scratch_alloc(arena, re->ncap, sizeof *re->cur, alignof(size_t));
    re->best = scratch_alloc(arena, re->ncap, sizeof *re->best, alignof(size_t));
    for (size_t i = 0; i < 2; i += 1) {
        re->lists[i].pc = scratch_alloc(arena, n, sizeof *re->lists[i].pc, alignof(uint16_t));
        re->lists[i].caps = scratch_alloc(arena, n * re->ncap, sizeof *re->lists[i].caps, alignof(size_t));
        if (re->lists[i].pc == NULL || re->lists[i].caps == NULL) return false;
    }
    if (re->visited == NULL || re->stack == NULL || re->cur == NULL || re->best == NULL) return false;
    memset(re->visited, 0, n * sizeof *re->visited);
    return true;
}

pb_regex_error pb_regex_compile(pb_arena *arena, pb_slice pattern, pb_regex **out, size_t *err_offset, char *err_msg, size_t err_msg_cap) {
    pb_arena scratch = {0};
    pb_rx_ctx c = {.arena = arena, .scratch = &scratch, .err = PB_REGEX_OK};
    pb_rx_inst prog[PB_REGEX_MAX_PROGRAM];
    pb_regex *re = NULL;
    pb_rx_node *root = NULL;
    char *copy = NULL;
    c.prog = prog;
    if (out != NULL) *out = NULL;

    if (pattern.len > PB_REGEX_MAX_PATTERN) {
        rx_fail(&c, PB_REGEX_TOO_LONG, PB_REGEX_MAX_PATTERN, "pattern too long");
        goto fail;
    }
    // The parser reads the caller's bytes directly; the arena copy is made once the pattern is good.
    c.src = pattern.len > 0 ? pattern.ptr : "";
    c.len = pattern.len;

    parse_flag_prefix(&c);
    root = parse_alt(&c, 0);
    if (root == NULL) goto fail;
    if (c.pos < c.len) { // parse_alt only stops early at ')'
        rx_fail(&c, PB_REGEX_SYNTAX, c.pos, "unmatched )");
        goto fail;
    }
    if (!emit_node(&c, root)) goto fail;
    if (emit_inst(&c, OP_MATCH, 0, 0, 0, NULL, c.len) < 0) goto fail;

    copy = pb_arena_alloc(arena, pattern.len + 1, 1);
    re = pb_arena_alloc(arena, sizeof *re, alignof(pb_regex));
    if (copy == NULL || re == NULL) {
        rx_fail(&c, PB_REGEX_OOM, 0, "out of memory");
        goto fail;
    }
    if (pattern.len > 0) memcpy(copy, pattern.ptr, pattern.len);
    copy[pattern.len] = '\0';
    memset(re, 0, sizeof *re);
    re->source = (pb_slice){copy, c.len};
    re->nprog = c.pc;
    re->prog = pb_arena_alloc(arena, c.pc * sizeof prog[0], alignof(pb_rx_inst));
    if (re->prog != NULL) memcpy(re->prog, prog, c.pc * sizeof prog[0]);
    re->ngroups = c.ngroups;
    re->ncap = 2 * (c.ngroups + 1);
    re->anchored = starts_anchored(root);
    re->first_byte = re->anchored ? -1 : required_first_byte(root);
    if (re->prog != NULL) compute_first_set(re);
    re->max_len = max_match_len(root);
    re->use_window = !re->anchored;
    if (re->prog != NULL) re->dfa = pb_rx_dfa_build(arena, re->prog, re->nprog, re->anchored);
    if (re->prog == NULL || !regex_alloc_scratch(arena, re)) {
        rx_fail(&c, PB_REGEX_OOM, 0, "out of memory");
        goto fail;
    }
    pb_arena_free(&scratch);
    if (out != NULL) *out = re;
    return PB_REGEX_OK;

fail:
    pb_arena_free(&scratch);
    if (err_offset != NULL) *err_offset = c.err_off;
    copy_message(err_msg, err_msg_cap, c.err_msg != NULL ? c.err_msg : "error");
    return c.err;
}

pb_slice pb_regex_source(const pb_regex *re) { return re->source; }
size_t pb_regex_program_len(const pb_regex *re) { return re->nprog; }
size_t pb_regex_group_count(const pb_regex *re) { return re->ngroups; }

// ---------- matcher ----------

static uint32_t vm_next_gen(pb_regex *re) {
    if (re->gen == UINT32_MAX) {
        memset(re->visited, 0, re->nprog * sizeof *re->visited);
        re->gen = 0;
    }
    re->gen += 1;
    return re->gen;
}

static bool vm_assertion_holds(pb_rx_op op, pb_slice text, size_t pos) {
    switch (op) {
    case OP_BOL: return pos == 0;
    case OP_EOL: return pos == text.len;
    default: {
        bool before = pos > 0 && pb_rx_is_word_byte((unsigned char)text.ptr[pos - 1]);
        bool after = pos < text.len && pb_rx_is_word_byte((unsigned char)text.ptr[pos]);
        return (before != after) == (op == OP_WORDB);
    }
    }
}

// Follows epsilon edges from pc0 at pos (explicit stack), appending consuming/MATCH
// instructions to `list` with captures from re->cur. Test mode (nc == 0) returns
// true at the first MATCH. Visited marks give one visit per pc per position, which
// also ends empty loops.
static bool vm_add_thread(pb_regex *re, pb_rx_list *list, unsigned pc0, size_t pos, pb_slice text, size_t nc, bool test) {
    pb_rx_frame *stack = re->stack;
    size_t sp = 0;
    stack[sp++] = (pb_rx_frame){0, (uint16_t)pc0, -1};
    while (sp > 0) {
        pb_rx_frame f = stack[--sp];
        if (f.slot >= 0) {
            re->cur[f.slot] = f.old;
            continue;
        }
        unsigned pc = f.pc;
        for (;;) {
            if (re->visited[pc] == re->gen) break;
            re->visited[pc] = re->gen;
            const pb_rx_inst *in = &re->prog[pc];
            if (in->op == OP_JMP) {
                pc = in->x;
            } else if (in->op == OP_SPLIT) {
                stack[sp++] = (pb_rx_frame){0, in->y, -1};
                pc = in->x;
            } else if (in->op == OP_SAVE) {
                if (in->x < nc) {
                    stack[sp++] = (pb_rx_frame){re->cur[in->x], 0, (int16_t)in->x};
                    re->cur[in->x] = pos;
                }
                pc += 1;
            } else if (in->op == OP_BOL || in->op == OP_EOL || in->op == OP_WORDB || in->op == OP_NWORDB) {
                if (!vm_assertion_holds((pb_rx_op)in->op, text, pos)) break;
                pc += 1;
            } else {
                if (in->op == OP_MATCH && test) return true;
                if (list->n >= re->nprog) break; // hard guard, never write past the arrays
                list->pc[list->n] = (uint16_t)pc;
                if (nc > 0) memcpy(list->caps + list->n * nc, re->cur, nc * sizeof(size_t));
                list->n += 1;
                break;
            }
        }
    }
    return false;
}

static bool vm_consumes(const pb_rx_inst *in, unsigned char byte) {
    return in->op == OP_CHAR ? in->byte == byte : pb_rx_set_has(in->set, byte);
}

// Runs the VM from `from`. Test mode (nc == 0) stops at the first MATCH; else the
// leftmost-first match's first nc slots end up in re->best. Each position costs
// clist->n + 1 steps of *budget (NULL = unlimited); GAVE_UP when it runs out.
static pb_regex_result vm_run(pb_regex *re, pb_slice text, size_t from, size_t nc, bool test, size_t *budget) {
    if (re->anchored && from > 0) return PB_REGEX_NO_MATCH;
    pb_rx_list *clist = &re->lists[0], *nlist = &re->lists[1];
    clist->n = 0;
    nlist->n = 0;
    bool matched = false;
    pb_regex_result result = PB_REGEX_NO_MATCH;
    size_t left = budget != NULL ? *budget : SIZE_MAX;
    size_t pos = from;
    vm_next_gen(re);
    for (;;) {
        if (!matched && clist->n == 0 && re->first_byte >= 0) {
            if (pos >= text.len) break;
            const char *hit = memchr(text.ptr + pos, re->first_byte, text.len - pos);
            if (hit == NULL) break;
            size_t hit_pos = (size_t)(hit - text.ptr);
            if (hit_pos != pos) {
                pos = hit_pos;
                vm_next_gen(re);
            }
        }
        if (!matched && clist->n == 0 && re->first_byte < 0 && re->use_first_set) {
            // Nothing alive and no empty match possible: skip bytes that cannot start one (uncharged).
            size_t p = pos;
            while (p < text.len && !re->first_ok[(unsigned char)text.ptr[p]]) p += 1;
            if (p >= text.len) break;
            if (p != pos) {
                pos = p;
                vm_next_gen(re);
            }
        }
        // Seed after older threads so earlier starts keep priority. Invariant: all
        // appends to one thread list share a generation, so this seed reuses clist's
        // (no vm_next_gen between); that keeps list->n <= nprog.
        if (!matched && (!re->anchored || pos == from)) {
            for (size_t i = 0; i < nc; i += 1) re->cur[i] = RX_UNSET;
            if (nc > 0) re->cur[0] = pos;
            if (vm_add_thread(re, clist, 0, pos, text, nc, test)) {
                result = PB_REGEX_MATCH;
                goto done;
            }
        }
        if (clist->n == 0 && (matched || re->anchored)) break;

        size_t cost = clist->n + 1;
        if (left < cost) {
            left = 0;
            result = PB_REGEX_GAVE_UP;
            goto done;
        }
        left -= cost;

        nlist->n = 0;
        vm_next_gen(re);
        for (size_t i = 0; i < clist->n; i += 1) {
            const pb_rx_inst *in = &re->prog[clist->pc[i]];
            if (in->op == OP_MATCH) {
                if (nc > 0) {
                    memcpy(re->best, clist->caps + i * nc, nc * sizeof(size_t));
                    re->best[1] = pos;
                }
                matched = true;
                break;
            }
            if (pos < text.len && vm_consumes(in, (unsigned char)text.ptr[pos])) {
                if (nc > 0) memcpy(re->cur, clist->caps + i * nc, nc * sizeof(size_t));
                if (vm_add_thread(re, nlist, clist->pc[i] + 1u, pos + 1, text, nc, test)) {
                    result = PB_REGEX_MATCH;
                    goto done;
                }
            }
        }
        pb_rx_list *swap = clist;
        clist = nlist;
        nlist = swap;
        if (pos >= text.len) break;
        pos += 1;
    }
    if (matched) result = PB_REGEX_MATCH;
done:
    if (budget != NULL) *budget = left;
    return result;
}

pb_regex_result pb_regex_test(pb_regex *re, pb_slice text, size_t *budget) {
    if (re->dfa != NULL) {
        size_t unused_end = 0;
        return pb_rx_dfa_test_end(re->dfa, text, 0, &unused_end) ? PB_REGEX_MATCH : PB_REGEX_NO_MATCH;
    }
    return vm_run(re, text, 0, 0, true, budget);
}

bool pb_regex_has_dfa(const pb_regex *re) { return re->dfa != NULL; }
size_t pb_regex_dfa_states(const pb_regex *re) { return re->dfa != NULL ? pb_rx_dfa_state_count(re->dfa) : 0; }
void pb_regex_disable_dfa(pb_regex *re) { re->dfa = NULL; }
void pb_regex_disable_first_set(pb_regex *re) { re->use_first_set = false; }
void pb_regex_disable_window(pb_regex *re) { re->use_window = false; }
void pb_regex_set_generation(pb_regex *re, uint32_t gen) { re->gen = gen; }

pb_regex_result pb_regex_search(pb_regex *re, pb_slice text, size_t from, pb_regex_span *spans, size_t spans_cap, size_t *budget) {
    if (from > text.len) return PB_REGEX_NO_MATCH;
    // Let the DFA reject non-matching text before the capture-tracking VM.
    size_t first_end = from;
    if (re->dfa != NULL && !pb_rx_dfa_test_end(re->dfa, text, from, &first_end)) return PB_REGEX_NO_MATCH;
    if (spans == NULL || spans_cap == 0) return re->dfa != NULL ? PB_REGEX_MATCH : vm_run(re, text, from, 0, true, budget);
    size_t want = spans_cap < re->ngroups + 1 ? spans_cap : re->ngroups + 1;
    // Window: no match starts before first_end - max_len, so the VM can begin there
    // (it still reads the whole text for ^ \b). Anchored patterns never move.
    size_t start = from;
    if (re->dfa != NULL && re->use_window && re->max_len != SIZE_MAX && first_end > from && first_end - from > re->max_len) {
        start = first_end - re->max_len;
    }
    pb_regex_result r = vm_run(re, text, start, 2 * want, false, budget);
    if (r != PB_REGEX_MATCH) return r;
    for (size_t i = 0; i < want; i += 1) {
        size_t s = re->best[2 * i], e = re->best[2 * i + 1];
        bool ok = s != RX_UNSET && e != RX_UNSET && s <= e;
        spans[i] = (pb_regex_span){ok ? s : 0, ok ? e : 0, ok};
    }
    return PB_REGEX_MATCH;
}
