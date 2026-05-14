#define _POSIX_C_SOURCE 200809L

#include "pb_program.h"

#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct publish_ctx {
    mb_router *router;
} publish_ctx;

static bool slice_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return false;
    }
    const long size = ftell(f);
    if (size < 0) {
        perror("ftell");
        fclose(f);
        return false;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    const size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return false;
    }
    buf[nread] = '\0';
    *out = buf;
    *out_len = nread;
    return true;
}

static bool rule_vec_append(pb_program *program, pb_rule rule) {
    if (program->len == program->cap) {
        const size_t next = program->cap == 0 ? 8 : program->cap * 2;
        pb_rule *rules = realloc(program->rules, next * sizeof rules[0]);
        if (rules == NULL) {
            return false;
        }
        program->rules = rules;
        program->cap = next;
    }
    program->rules[program->len] = rule;
    program->len += 1;
    return true;
}

static bool load_on_form(pb_program *program, pb_value form) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "patchbay: top-level form must be a list headed by a symbol\n");
        return false;
    }
    pb_values items = form.seq;
    if (!slice_eq(items.items[0].text, "on")) {
        return true;
    }
    if (items.len < 3 || items.items[1].kind != PB_STRING) {
        fprintf(stderr, "patchbay: invalid on form\n");
        return false;
    }

    pb_rule rule = {
        .filter = items.items[1].text,
        .body = items.items[items.len - 1],
    };
    const size_t body_idx = items.len - 1;
    size_t i = 2;
    while (i < body_idx) {
        if (i + 1 >= body_idx || items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "patchbay: invalid on options\n");
            return false;
        }
        if (!slice_eq(items.items[i].text, "reentrant")) {
            fprintf(stderr, "patchbay: unknown on option: %.*s\n",
                    (int)items.items[i].text.len, items.items[i].text.ptr);
            return false;
        }
        if (items.items[i + 1].kind != PB_BOOL) {
            fprintf(stderr, "patchbay: :reentrant expects boolean\n");
            return false;
        }
        rule.reentrant = items.items[i + 1].boolean;
        i += 2;
    }
    return rule_vec_append(program, rule);
}

bool pb_program_load_file(pb_program *program, const char *path) {
    *program = (pb_program){0};
    char *source = NULL;
    size_t source_len = 0;
    if (!read_file(path, &source, &source_len)) {
        return false;
    }

    const pb_parse_result parsed = pb_parse_patchbay_source(&program->parse_arena, path, source, source_len);
    free(source);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "patchbay: parse error: %s at byte %zu\n",
                pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_program_free(program);
        return false;
    }
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!load_on_form(program, parsed.forms.items[i])) {
            pb_program_free(program);
            return false;
        }
    }
    fprintf(stderr, "info: loaded %zu patchbay form(s)\n", program->len);
    return true;
}

void pb_program_free(pb_program *program) {
    for (size_t i = 0; i < program->len; i += 1) {
        pb_eval_state_free(&program->rules[i].state);
    }
    free(program->rules);
    pb_arena_free(&program->scratch);
    pb_arena_free(&program->parse_arena);
    *program = (pb_program){0};
}

static bool token_match(pb_slice filter, pb_slice subject) {
    size_t fp = 0;
    size_t sp = 0;

    for (;;) {
        const size_t fs = fp;
        while (fp < filter.len && filter.ptr[fp] != '.') fp += 1;
        const pb_slice ftok = {.ptr = filter.ptr + fs, .len = fp - fs};

        if (ftok.len == 1 && ftok.ptr[0] == '>') {
            return true;
        }

        const size_t ss = sp;
        while (sp < subject.len && subject.ptr[sp] != '.') sp += 1;
        if (ss == subject.len && ftok.len != 0) {
            return false;
        }
        const pb_slice stok = {.ptr = subject.ptr + ss, .len = sp - ss};

        if (!(ftok.len == 1 && ftok.ptr[0] == '*') &&
            !(ftok.len == stok.len && memcmp(ftok.ptr, stok.ptr, ftok.len) == 0)) {
            return false;
        }

        const bool fend = fp == filter.len;
        const bool send = sp == subject.len;
        if (fend || send) {
            return fend && send;
        }
        fp += 1;
        sp += 1;
    }
}

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    publish_ctx *p = ctx;
    return mb_router_publish(p->router,
                             (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len},
                             (mb_slice){.ptr = (const uint8_t *)payload.ptr, .len = payload.len});
}

bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    const pb_slice pb_subject = {.ptr = (const char *)subject.ptr, .len = subject.len};
    const pb_slice pb_payload = {.ptr = (const char *)payload.ptr, .len = payload.len};
    publish_ctx pub = {.router = router};
    bool ok = true;

    for (size_t i = 0; i < program->len; i += 1) {
        pb_rule *rule = &program->rules[i];
        if (!token_match(rule->filter, pb_subject)) {
            continue;
        }
        pb_eval_ctx ctx = {
            .arena = &program->scratch,
            .state = &rule->state,
            .rule_id = i,
            .subject = pb_subject,
            .payload = pb_payload,
            .publish = publish_cb,
            .publish_ctx = &pub,
        };
        const pb_eval_result r = pb_eval(&ctx, rule->body);
        pb_arena_reset(&program->scratch);
        if (r.err != PB_EVAL_OK) {
            fprintf(stderr, "patchbay: rule %zu eval failed: %s\n", i, pb_eval_error_name(r.err));
            ok = false;
        }
    }
    return ok;
}
