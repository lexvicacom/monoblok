#include "pb_json.h"

#include "fs.h"

#include <stdio.h>

static void dump_value(const pb_value *v, int depth);

static void indent(int depth) {
    for (int i = 0; i < depth; i += 1) {
        fputs("  ", stdout);
    }
}

static void dump_text(const char *tag, pb_slice s) {
    printf("%s %.*s\n", tag, (int)s.len, s.ptr);
}

static void dump_seq(const char *tag, pb_values seq, int depth) {
    printf("%s len=%zu\n", tag, seq.len);
    for (size_t i = 0; i < seq.len; i += 1) {
        dump_value(&seq.items[i], depth + 1);
    }
}

static void dump_value(const pb_value *v, int depth) {
    indent(depth);
    switch (v->kind) {
    case PB_NIL: puts("nil"); break;
    case PB_BOOL: printf("bool %s\n", v->boolean ? "true" : "false"); break;
    case PB_NUMBER: printf("number %.17g\n", v->number); break;
    case PB_SYMBOL: dump_text("symbol", v->text); break;
    case PB_KEYWORD: dump_text("keyword", v->text); break;
    case PB_STRING: dump_text("string", v->text); break;
    case PB_LIST: dump_seq("list", v->seq, depth); break;
    case PB_VECTOR: dump_seq("vector", v->seq, depth); break;
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s patchbay.{edn,json}\n", argv[0]);
        return 2;
    }

    mb_buf src = {0};
    if (!mb_read_file(argv[1], &src)) {
        perror(argv[1]);
        return 1;
    }

    pb_arena arena = {0};
    const pb_parse_result r = pb_parse_patchbay_source(&arena, argv[1], (const char *)src.ptr, src.len);
    if (r.err != PB_PARSE_OK) {
        fprintf(stderr, "%s at byte %zu\n", pb_parse_error_name(r.err), r.err_offset);
        pb_arena_free(&arena);
        mb_buf_free(&src);
        return 1;
    }

    printf("forms len=%zu\n", r.forms.len);
    for (size_t i = 0; i < r.forms.len; i += 1) {
        dump_value(&r.forms.items[i], 0);
    }

    pb_arena_free(&arena);
    mb_buf_free(&src);
    return 0;
}
