#define _POSIX_C_SOURCE 200809L

#include "pb_program.h"

#include "pb_eval_internal.h"
#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct publish_ctx {
    pb_program *program;
    mb_router *router;
    uint64_t now_ms;
    int64_t wall_ms;
    size_t depth;
    bool reentrant;
} publish_ctx;

enum { PB_PROGRAM_MAX_REENTRY_DEPTH = 8 };

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

static bool value_head_eq(pb_value v, const char *lit) {
    return v.kind == PB_LIST &&
           v.seq.len > 0 &&
           v.seq.items[0].kind == PB_SYMBOL &&
           slice_eq(v.seq.items[0].text, lit);
}

static bool call_has_ms_window(pb_value v) {
    return v.kind == PB_LIST &&
           v.seq.len >= 3 &&
           v.seq.items[1].kind == PB_KEYWORD &&
           slice_eq(v.seq.items[1].text, "ms");
}

static void scan_clock_forms(pb_program *program, pb_value v) {
    if (value_head_eq(v, "now")) {
        program->uses_wall_clock = true;
    }
    if ((value_head_eq(v, "bar!") || value_head_eq(v, "bar") || value_head_eq(v, "moving-avg")) &&
        call_has_ms_window(v)) {
        program->uses_clock_timer = true;
    }
    if (v.kind == PB_LIST || v.kind == PB_VECTOR) {
        for (size_t i = 0; i < v.seq.len; i += 1) {
            scan_clock_forms(program, v.seq.items[i]);
        }
    }
}

static bool load_source(pb_program *program, const char *label, const char *source, size_t source_len, bool log) {
    *program = (pb_program){0};
    const pb_parse_result parsed = pb_parse_patchbay_source(&program->parse_arena, label, source, source_len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "patchbay: parse error in %s: %s at byte %zu\n",
                label, pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_program_free(program);
        return false;
    }
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!load_on_form(program, parsed.forms.items[i])) {
            pb_program_free(program);
            return false;
        }
        scan_clock_forms(program, parsed.forms.items[i]);
    }
    if (log) {
        fprintf(stderr, "info: loaded %zu patchbay form(s)\n", program->len);
        if (program->uses_wall_clock) {
            fprintf(stderr, "info: patchbay wallclock: enabled\n");
        }
        if (program->uses_clock_timer) {
            fprintf(stderr, "info: patchbay clock: enabled (one-shot deadlines)\n");
        }
    }
    return true;
}

bool pb_program_load_source(pb_program *program, const char *label, const char *source, size_t source_len) {
    return load_source(program, label, source, source_len, false);
}

bool pb_program_load_file(pb_program *program, const char *path) {
    char *source = NULL;
    size_t source_len = 0;
    if (!read_file(path, &source, &source_len)) {
        *program = (pb_program){0};
        return false;
    }

    const bool ok = load_source(program, path, source, source_len, true);
    free(source);
    return ok;
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

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                uint64_t now_ms, int64_t wall_ms, size_t depth);

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    publish_ctx *p = ctx;
    if (!mb_router_publish(p->router,
                           (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len},
                           (mb_slice){.ptr = (const uint8_t *)payload.ptr, .len = payload.len})) {
        return false;
    }
    if (!p->reentrant) {
        return true;
    }
    if (p->depth >= PB_PROGRAM_MAX_REENTRY_DEPTH) {
        fprintf(stderr, "patchbay: reentry depth cap reached\n");
        return true;
    }
    return eval_publish_slices(p->program, p->router, subject, payload, p->now_ms, p->wall_ms, p->depth + 1);
}

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                uint64_t now_ms, int64_t wall_ms, size_t depth) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    bool ok = true;
    for (size_t i = 0; i < program->len; i += 1) {
        pb_rule *rule = &program->rules[i];
        if (!token_match(rule->filter, subject)) {
            continue;
        }
        publish_ctx pub = {
            .program = program,
            .router = router,
            .now_ms = now_ms,
            .wall_ms = wall_ms,
            .depth = depth,
            .reentrant = rule->reentrant,
        };
        pb_eval_ctx ctx = {
            .arena = &program->scratch,
            .state = &rule->state,
            .rule_id = i,
            .now_ms = now_ms,
            .wall_ms = wall_ms,
            .subject = subject,
            .payload = payload,
            .publish = publish_cb,
            .publish_ctx = &pub,
        };
        const pb_eval_result r = pb_eval(&ctx, rule->body);
        if (r.err != PB_EVAL_OK) {
            fprintf(stderr, "patchbay: rule %zu eval failed: %s\n", i, pb_eval_error_name(r.err));
            ok = false;
        }
    }
    return ok;
}

bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms) {
    if (program == NULL || program->len == 0) {
        return true;
    }
    const bool outer = program->eval_depth == 0;
    program->eval_depth += 1;
    const bool ok = eval_publish_slices(program, router,
                                        (pb_slice){.ptr = (const char *)subject.ptr, .len = subject.len},
                                        (pb_slice){.ptr = (const char *)payload.ptr, .len = payload.len},
                                        now_ms, wall_ms, 0);
    program->eval_depth -= 1;
    if (outer) {
        pb_arena_reset(&program->scratch);
    }
    return ok;
}

bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    const bool outer = program->eval_depth == 0;
    program->eval_depth += 1;
    bool ok = true;
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1) {
        pb_rule *rule = &program->rules[rule_idx];
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1) {
            pb_eval_state_entry *entry = &rule->state.items[state_idx];
            publish_ctx pub = {
                .program = program,
                .router = router,
                .now_ms = now_ms,
                .wall_ms = wall_ms,
                .depth = 0,
                .reentrant = rule->reentrant,
            };
            pb_eval_ctx ctx = {
                .arena = &program->scratch,
                .state = &rule->state,
                .rule_id = rule_idx,
                .now_ms = now_ms,
                .wall_ms = wall_ms,
                .subject = {.ptr = entry->subject, .len = entry->subject_len},
                .payload = {.ptr = "", .len = 0},
                .publish = publish_cb,
                .publish_ctx = &pub,
            };
            const pb_eval_result r = pb_eval_tick_state_entry(&ctx, entry);
            if (r.err != PB_EVAL_OK) {
                fprintf(stderr, "patchbay: rule %zu clock tick failed: %s\n",
                        rule_idx, pb_eval_error_name(r.err));
                ok = false;
            }
        }
    }
    program->eval_depth -= 1;
    if (outer) {
        pb_arena_reset(&program->scratch);
    }
    return ok;
}

static bool entry_deadline(const pb_eval_state_entry *entry, uint64_t *out_ms) {
    if (entry->kind == PB_EVAL_STATE_RING && entry->ring_time_window && entry->ring_len > 0) {
        const size_t idx = entry->ring_start;
        *out_ms = entry->ring_times_ms[idx] + entry->ring_window_ms;
        return true;
    }
    if (entry->kind == PB_EVAL_STATE_BAR && entry->bar_time_window && entry->bar_count > 0) {
        *out_ms = entry->bar_window_start_ms + entry->bar_window_ms;
        return true;
    }
    return false;
}

bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms) {
    bool found = false;
    uint64_t best = 0;
    if (program == NULL || !program->uses_clock_timer) {
        return false;
    }
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1) {
        const pb_rule *rule = &program->rules[rule_idx];
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1) {
            uint64_t deadline = 0;
            if (entry_deadline(&rule->state.items[state_idx], &deadline) && (!found || deadline < best)) {
                best = deadline;
                found = true;
            }
        }
    }
    if (found) {
        *out_ms = best;
    }
    return found;
}
