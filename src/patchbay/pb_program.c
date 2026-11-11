#define _POSIX_C_SOURCE 200809L

#include "pb_program.h"

#include "array.h"
#include "fs.h"
#include "pb_config.h"
#include "pb_eval_internal.h"
#include "pb_json.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct publish_ctx {
    pb_program *program;
    mb_router *router;
    uint64_t now_ms;
    int64_t wall_ms;
    size_t rule_idx;
    size_t depth;
    bool reentrant;
    bool replaying;
} publish_ctx;

enum {
    PB_PROGRAM_SCRATCH_RETAIN_BYTES = 4 * 1024 * 1024,
};

static void reset_scratch(pb_program *program) {
    pb_arena_reset(&program->scratch);
    pb_arena_trim(&program->scratch, PB_PROGRAM_SCRATCH_RETAIN_BYTES);
}

static bool slice_match(pb_slice filter, pb_slice subject) {
    return mb_router_subject_matches((mb_slice){.ptr = (const uint8_t *)filter.ptr, .len = filter.len},
                                     (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len});
}

static bool rule_vec_append(pb_program *program, pb_rule rule) {
    if (!mb_array_reserve((void **)&program->rules, &program->cap, program->len + 1,
                          sizeof program->rules[0], 8)) {
        return false;
    }
    program->rules[program->len] = rule;
    program->len += 1;
    return true;
}

bool pb_every_parse(pb_arena *arena, pb_values items, bool allow_user_call, bool allow_user_symbol,
                    pb_every_spec *out, char *err, size_t err_cap) {
    if (items.len < 4 || items.items[1].kind != PB_KEYWORD || !pb_slice_eq_lit(items.items[1].text, "ms")) {
        snprintf(err, err_cap, "every expects (every :ms N [:reentrant BOOL] BODY...)");
        return false;
    }
    const pb_value ms = items.items[2];
    if (ms.kind != PB_NUMBER || ms.number < 1 || ms.number > 1e12 || ms.number != (double)(uint64_t)ms.number) {
        snprintf(err, err_cap, "every :ms must be an integer from 1 to 1000000000000");
        return false;
    }
    pb_every_spec spec = {.ms = (uint64_t)ms.number};
    size_t i = 3;
    while (i < items.len && items.items[i].kind == PB_KEYWORD) {
        if (!pb_slice_eq_lit(items.items[i].text, "reentrant")) {
            snprintf(err, err_cap, "unknown every option: %.*s", (int)items.items[i].text.len, items.items[i].text.ptr);
            return false;
        }
        if (i + 1 >= items.len || items.items[i + 1].kind != PB_BOOL) {
            snprintf(err, err_cap, ":reentrant expects boolean");
            return false;
        }
        spec.reentrant = items.items[i + 1].boolean;
        i += 2;
    }
    if (i >= items.len) {
        snprintf(err, err_cap, "every needs a body");
        return false;
    }
    spec.body = pb_arena_alloc(arena, sizeof *spec.body, alignof(pb_value));
    if (spec.body == NULL) {
        snprintf(err, err_cap, "out of memory");
        return false;
    }
    if (items.len - i == 1) {
        *spec.body = items.items[i];
    } else {
        const size_t n = items.len - i;
        pb_value *list = pb_arena_alloc(arena, (n + 1) * sizeof list[0], alignof(pb_value));
        if (list == NULL) {
            snprintf(err, err_cap, "out of memory");
            return false;
        }
        list[0] = (pb_value){.kind = PB_SYMBOL, .text = {.ptr = "do", .len = 2}};
        memcpy(list + 1, items.items + i, n * sizeof list[0]);
        *spec.body = (pb_value){.kind = PB_LIST, .seq = {.items = list, .len = n + 1}};
    }
    if (!pb_resolve_rule(arena, spec.body, allow_user_call, allow_user_symbol, err, err_cap)) {
        return false;
    }
    *out = spec;
    return true;
}

static bool load_every_form(pb_program *program, pb_values items) {
    pb_every_spec spec = {0};
    char err[160];
    if (!pb_every_parse(&program->parse_arena, items, program->user_call != NULL, program->user_symbol != NULL,
                        &spec, err, sizeof err)) {
        fprintf(stderr, "patchbay: rule %zu (every): %s\n", program->len, err);
        return false;
    }
    program->uses_clock_timer = true;
    return rule_vec_append(program, (pb_rule){
                                        .filter = {.ptr = "", .len = 0},
                                        .body = *spec.body,
                                        .every_ms = spec.ms,
                                        .reentrant = spec.reentrant,
                                    });
}

static bool load_on_form(pb_program *program, pb_value form) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "patchbay: top-level form must be a list headed by a symbol\n");
        return false;
    }
    pb_values items = form.seq;
    if (pb_config_head_is(items.items[0].text)) {
        const pb_config_ctx config_ctx = {.arena = &program->parse_arena, .prefix = "patchbay"};
        return pb_config_load_form(&config_ctx, items, &program->lvc, &program->bridge, &program->importer);
    }
    if (pb_slice_eq_lit(items.items[0].text, "every")) {
        return load_every_form(program, items);
    }
    if (!pb_slice_eq_lit(items.items[0].text, "on")) {
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
        if (!pb_slice_eq_lit(items.items[i].text, "reentrant")) {
            fprintf(stderr, "patchbay: unknown on option: %.*s\n", (int)items.items[i].text.len,
                    items.items[i].text.ptr);
            return false;
        }
        if (items.items[i + 1].kind != PB_BOOL) {
            fprintf(stderr, "patchbay: :reentrant expects boolean\n");
            return false;
        }
        rule.reentrant = items.items[i + 1].boolean;
        i += 2;
    }
    char resolve_err[160];
    if (!pb_resolve_rule(&program->parse_arena, &rule.body, program->user_call != NULL, program->user_symbol != NULL,
                         resolve_err, sizeof resolve_err)) {
        fprintf(stderr, "patchbay: rule %zu (\"%.*s\"): %s\n", program->len, (int)rule.filter.len, rule.filter.ptr,
                resolve_err);
        return false;
    }
    return rule_vec_append(program, rule);
}

static bool first_subject_token(pb_slice s, pb_slice *tok) {
    size_t end = 0;
    while (end < s.len && s.ptr[end] != '.') {
        end += 1;
    }
    if (end == 0) {
        return false;
    }
    *tok = (pb_slice){.ptr = s.ptr, .len = end};
    return true;
}

static bool wildcard_token(pb_slice tok) {
    return tok.len == 1 && (tok.ptr[0] == '*' || tok.ptr[0] == '>');
}

static bool rule_ref_append(pb_rule_ref_list *list, size_t rule_idx) {
    if (!mb_array_reserve((void **)&list->items, &list->cap, list->len + 1,
                          sizeof list->items[0], 8)) {
        return false;
    }
    list->items[list->len] = rule_idx;
    list->len += 1;
    return true;
}

static pb_rule_bucket *rule_bucket(pb_program *program, pb_slice key) {
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        if (pb_slice_eq(program->rule_buckets[i].key, key)) {
            return &program->rule_buckets[i];
        }
    }
    if (!mb_array_reserve((void **)&program->rule_buckets, &program->rule_bucket_cap,
                          program->rule_bucket_len + 1, sizeof program->rule_buckets[0], 8)) {
        return NULL;
    }
    pb_rule_bucket *bucket = &program->rule_buckets[program->rule_bucket_len];
    *bucket = (pb_rule_bucket){.key = key};
    program->rule_bucket_len += 1;
    return bucket;
}

static void rule_index_free(pb_program *program) {
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        free(program->rule_buckets[i].rules.items);
    }
    free(program->rule_buckets);
    free(program->rule_global.items);
    program->rule_buckets = NULL;
    program->rule_bucket_len = 0;
    program->rule_bucket_cap = 0;
    program->rule_global = (pb_rule_ref_list){0};
}

static bool build_rule_index(pb_program *program) {
    rule_index_free(program);
    for (size_t i = 0; i < program->len; i += 1) {
        if (program->rules[i].every_ms != 0) {
            continue; // every rules have no filter and never see publishes
        }
        pb_slice first = {0};
        if (!first_subject_token(program->rules[i].filter, &first) || wildcard_token(first)) {
            if (!rule_ref_append(&program->rule_global, i)) {
                return false;
            }
            continue;
        }
        pb_rule_bucket *bucket = rule_bucket(program, first);
        if (bucket == NULL || !rule_ref_append(&bucket->rules, i)) {
            return false;
        }
    }
    return true;
}

static bool value_head_eq(pb_value v, const char *lit) {
    return v.kind == PB_LIST && v.seq.len > 0 && v.seq.items[0].kind == PB_SYMBOL && pb_slice_eq_lit(v.seq.items[0].text, lit);
}

static bool call_has_ms_window(pb_value v) {
    return v.kind == PB_LIST && v.seq.len >= 3 && v.seq.items[1].kind == PB_KEYWORD &&
           pb_slice_eq_lit(v.seq.items[1].text, "ms");
}

// Sets uses_wall_clock / uses_clock_timer from the form table flags.
static void scan_clock_forms(pb_program *program, pb_value v) {
    if (v.kind == PB_LIST && v.seq.len > 0 && v.seq.items[0].kind == PB_SYMBOL) {
        const pb_form_entry *entry = pb_form_lookup(v.seq.items[0].text);
        if (entry != NULL) {
            if (entry->form == PB_FORM_NOW) {
                program->uses_wall_clock = true;
            }
            if (entry->clocked || (entry->clocked_ms && call_has_ms_window(v))) {
                program->uses_clock_timer = true;
            }
        }
    }
    if (v.kind == PB_LIST || v.kind == PB_VECTOR) {
        for (size_t i = 0; i < v.seq.len; i += 1) {
            scan_clock_forms(program, v.seq.items[i]);
        }
    }
}

static size_t count_print_forms(pb_value v) {
    size_t n = 0;
    if (v.kind == PB_LIST && v.seq.len > 0 && v.seq.items[0].kind == PB_SYMBOL) {
        const pb_form_entry *entry = pb_form_lookup(v.seq.items[0].text);
        n = entry != NULL && entry->form == PB_FORM_PRINT ? 1 : 0;
    }
    if (v.kind == PB_LIST || v.kind == PB_VECTOR) {
        for (size_t i = 0; i < v.seq.len; i += 1) {
            n += count_print_forms(v.seq.items[i]);
        }
    }
    return n;
}

static size_t count_rule_print_forms(pb_value v) {
    if (value_head_eq(v, "every")) {
        return count_print_forms(v);
    }
    if (!value_head_eq(v, "on") || v.seq.len < 3) {
        return 0;
    }
    return count_print_forms(v.seq.items[v.seq.len - 1]);
}

static bool load_source(pb_program *program, const char *label, const char *source, size_t source_len, bool log) {
    pb_eval_symbol_fn user_symbol = program->user_symbol;
    pb_eval_call_fn user_call = program->user_call;
    void *user_ctx = program->user_ctx;
    *program = (pb_program){
        .user_symbol = user_symbol,
        .user_call = user_call,
        .user_ctx = user_ctx,
    };
    const pb_parse_result parsed = pb_parse_patchbay_source(&program->parse_arena, label, source, source_len);
    if (parsed.err != PB_PARSE_OK) {
        size_t line = 0;
        size_t col = 0;
        pb_line_col(source, source_len, parsed.err_offset, &line, &col);
        fprintf(stderr, "patchbay: parse error in %s:%zu:%zu: %s%s%s (byte %zu)\n", label, line, col,
                pb_parse_error_name(parsed.err), parsed.err_msg[0] != '\0' ? ": " : "", parsed.err_msg, parsed.err_offset);
        pb_program_free(program);
        return false;
    }
    size_t print_calls = 0;
    size_t print_rules = 0;
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!load_on_form(program, parsed.forms.items[i])) {
            pb_program_free(program);
            return false;
        }
        scan_clock_forms(program, parsed.forms.items[i]);
        const size_t form_prints = count_rule_print_forms(parsed.forms.items[i]);
        if (form_prints != 0) {
            print_calls += form_prints;
            print_rules += 1;
        }
    }
    if (!build_rule_index(program)) {
        pb_program_free(program);
        return false;
    }
    // Every slots point at the rule's own body; the rules array no longer moves.
    for (size_t i = 0; i < program->len; i += 1) {
        pb_rule *rule = &program->rules[i];
        if (rule->every_ms != 0 && !pb_eval_every_arm(&rule->state, i, rule->every_ms, &rule->body)) {
            pb_program_free(program);
            return false;
        }
    }
    if (log) {
        fprintf(stderr, "info: loaded %zu patchbay form(s) from %s\n", program->len, label);
        if (program->uses_wall_clock) {
            fprintf(stderr, "info: patchbay wallclock: enabled\n");
        }
        if (program->uses_clock_timer) {
            fprintf(stderr, "info: patchbay timers: enabled (one-shot deadlines)\n");
        }
        if (print_calls != 0) {
            fprintf(stderr,
                    "warning: patchbay contains %zu print! call(s) across %zu rule(s); will log payload data to stderr and add per-call overhead (debug aid, do not leave in production)\n",
                    print_calls, print_rules);
        }
    }
    return true;
}

bool pb_program_load_source(pb_program *program, const char *label, const char *source, size_t source_len) {
    return load_source(program, label, source, source_len, false);
}

void pb_program_set_eval_hooks(pb_program *program, pb_eval_symbol_fn user_symbol, pb_eval_call_fn user_call, void *user_ctx) {
    if (program == NULL) {
        return;
    }
    program->user_symbol = user_symbol;
    program->user_call = user_call;
    program->user_ctx = user_ctx;
}

bool pb_program_load_file(pb_program *program, const char *path) {
    mb_buf source = {0};
    if (!mb_read_file(path, &source)) {
        perror(path);
        mb_buf_free(&source);
        *program = (pb_program){0};
        return false;
    }

    const bool ok = load_source(program, path, (const char *)source.ptr, source.len, true);
    mb_buf_free(&source);
    return ok;
}

void pb_program_free(pb_program *program) {
    for (size_t i = 0; i < program->len; i += 1) {
        pb_eval_state_free(&program->rules[i].state);
    }
    rule_index_free(program);
    free(program->rules);
    pb_config_free(&program->lvc, &program->bridge, &program->importer);
    pb_arena_free(&program->scratch);
    pb_arena_free(&program->parse_arena);
    *program = (pb_program){0};
}

static const pb_rule_ref_list *rule_refs_for_subject(const pb_program *program, pb_slice subject) {
    pb_slice first = {0};
    if (!first_subject_token(subject, &first)) {
        return NULL;
    }
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        const pb_rule_bucket *bucket = &program->rule_buckets[i];
        if (pb_slice_eq(bucket->key, first)) {
            return &bucket->rules;
        }
    }
    return NULL;
}

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                pb_slice reply_to, uint64_t now_ms, int64_t wall_ms, size_t depth, bool replaying);

// `(last SUBJECT)` hook: reads what `$LVC.SUBJECT` would deliver. The payload
// view borrows router memory; the evaluator copies it before any publish.
static bool last_cb(void *ctx, pb_slice subject, pb_slice *payload) {
    const mb_router *router = ctx;
    mb_slice found = {0};
    if (router == NULL ||
        !mb_router_lvc_latest(router, (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len}, &found)) {
        return false;
    }
    *payload = (pb_slice){.ptr = (const char *)found.ptr, .len = found.len};
    return true;
}

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    publish_ctx *p = ctx;
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject) ||
        !mb_router_publish_with_options(p->router, mb_subject,
                                        (mb_slice){.ptr = (const uint8_t *)payload.ptr, .len = payload.len},
                                        (mb_router_publish_options){
                                            .replaying = p->replaying,
                                            .has_assumed_ts_ms = p->wall_ms >= 0,
                                            .assumed_ts_ms = p->wall_ms})) {
        return false;
    }
    if (p->program != NULL && p->rule_idx < p->program->len) {
        p->program->rules[p->rule_idx].publishes_emitted += 1;
    }
    if (!p->reentrant) {
        return true;
    }
    if (p->depth >= PB_PROGRAM_MAX_REENTRY_DEPTH) {
        fprintf(stderr, "patchbay: reentry depth cap reached\n");
        return true;
    }
    (void)eval_publish_slices(p->program, p->router, subject, payload, (pb_slice){.ptr = "", .len = 0}, p->now_ms, p->wall_ms, p->depth + 1, p->replaying);
    return true;
}

static bool eval_rule_for_publish(pb_program *program, mb_router *router, size_t rule_idx,
                                  pb_slice subject, pb_slice payload, pb_slice reply_to,
                                  uint64_t now_ms, int64_t wall_ms, size_t depth, bool replaying) {
    pb_rule *rule = &program->rules[rule_idx];
    if (rule->every_ms != 0 || !slice_match(rule->filter, subject)) {
        return true;
    }
    publish_ctx pub = {
        .program = program,
        .router = router,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .rule_idx = rule_idx,
        .depth = depth,
        .reentrant = rule->reentrant,
        .replaying = replaying,
    };
    pb_eval_ctx ctx = {
        .arena = &program->scratch,
        .state = &rule->state,
        .rule_id = rule_idx,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .replaying = replaying,
        .subject = subject,
        .payload = payload,
        .reply_to = reply_to,
        .publish = publish_cb,
        .publish_ctx = &pub,
        .last = last_cb,
        .last_ctx = router,
        .publishes_emitted = &rule->publishes_emitted,
        .publishes_suppressed = &rule->publishes_suppressed,
        .user_symbol = program->user_symbol,
        .user_call = program->user_call,
        .user_ctx = program->user_ctx,
    };
    const pb_eval_result r = pb_eval(&ctx, rule->body);
    if (r.err != PB_EVAL_OK) {
        // Log the 1st failure of a rule and then every power of two, so a bad
        // rule on a hot subject cannot flood stderr. No allocation here.
        rule->eval_failures += 1;
        if ((rule->eval_failures & (rule->eval_failures - 1)) == 0) {
            fprintf(stderr, "patchbay: rule %zu (\"%.*s\") eval failed: %s (failure #%llu)\n", rule_idx,
                    (int)rule->filter.len, rule->filter.ptr, pb_eval_error_name(r.err),
                    (unsigned long long)rule->eval_failures);
        }
        return false;
    }
    return true;
}

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                pb_slice reply_to, uint64_t now_ms, int64_t wall_ms, size_t depth, bool replaying) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    bool ok = true;
    const pb_rule_ref_list *bucket = rule_refs_for_subject(program, subject);
    size_t bucket_pos = 0;
    size_t global_pos = 0;
    while ((bucket != NULL && bucket_pos < bucket->len) || global_pos < program->rule_global.len) {
        const size_t bucket_idx = bucket != NULL && bucket_pos < bucket->len ? bucket->items[bucket_pos] : SIZE_MAX;
        const size_t global_idx = global_pos < program->rule_global.len ? program->rule_global.items[global_pos] : SIZE_MAX;
        size_t rule_idx = 0;
        if (bucket_idx < global_idx) {
            rule_idx = bucket_idx;
            bucket_pos += 1;
        } else {
            rule_idx = global_idx;
            global_pos += 1;
        }
        if (!eval_rule_for_publish(program, router, rule_idx, subject, payload, reply_to, now_ms, wall_ms, depth, replaying)) {
            ok = false;
        }
    }
    return ok;
}

bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms, pb_program_eval_options options) {
    if (program == NULL || program->len == 0) {
        return true;
    }
    const bool outer = program->eval_depth == 0;
    program->eval_depth += 1;
    const bool ok =
        eval_publish_slices(program, router, (pb_slice){.ptr = (const char *)subject.ptr, .len = subject.len},
                            (pb_slice){.ptr = (const char *)payload.ptr, .len = payload.len},
                            (pb_slice){.ptr = (const char *)options.reply_to.ptr, .len = options.reply_to.len}, now_ms,
                            wall_ms, 0, options.replaying);
    program->eval_depth -= 1;
    if (outer) {
        reset_scratch(program);
    }
    return ok;
}

// An every slot with deadline 0 has not been scheduled yet: its first deadline
// is now + ms, and it does not fire. Must run before any deadline is consulted.
static void schedule_every_slots(pb_program *program, uint64_t now_ms) {
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1) {
        pb_rule *rule = &program->rules[rule_idx];
        if (rule->every_ms == 0) {
            continue;
        }
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1) {
            pb_eval_state_entry *entry = &rule->state.items[state_idx];
            if (entry->clock_kind == PB_EVAL_CLOCK_EVERY && entry->clock_deadline_ms == 0) {
                entry->clock_deadline_ms = now_ms + entry->clock_interval_ms;
            }
        }
    }
}

bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                     pb_program_eval_options options) {
    if (program == NULL || program->len == 0) {
        return true;
    }
    schedule_every_slots(program, now_ms);

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
                .rule_idx = rule_idx,
                .depth = 0,
                .reentrant = rule->reentrant,
                .replaying = options.replaying,
            };
            pb_eval_ctx ctx = {
                .arena = &program->scratch,
                .state = &rule->state,
                .rule_id = rule_idx,
                .now_ms = now_ms,
                .wall_ms = wall_ms,
                .replaying = options.replaying,
                .subject = (pb_slice){.ptr = entry->subject, .len = entry->subject_len},
                .payload = {.ptr = "", .len = 0},
                .publish = publish_cb,
                .publish_ctx = &pub,
                .last = last_cb,
                .last_ctx = router,
                .publishes_emitted = &rule->publishes_emitted,
                .publishes_suppressed = &rule->publishes_suppressed,
                .user_symbol = program->user_symbol,
                .user_call = program->user_call,
                .user_ctx = program->user_ctx,
            };
            const pb_eval_result r = pb_eval_tick_state_entry(&ctx, entry);
            if (r.err != PB_EVAL_OK) {
                fprintf(stderr, "patchbay: rule %zu clock tick failed: %s\n", rule_idx, pb_eval_error_name(r.err));
                ok = false;
            }
        }
    }
    program->eval_depth -= 1;
    if (outer) {
        reset_scratch(program);
    }
    return ok;
}

bool pb_program_tick_until(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms,
                           pb_program_eval_options options) {
    bool ok = true;
    uint64_t deadline = 0;
    if (program != NULL) {
        schedule_every_slots(program, now_ms);
    }
    while (pb_program_next_clock_deadline(program, &deadline) && deadline <= now_ms) {
        int64_t tick_wall_ms = wall_ms;
        const uint64_t delta = now_ms - deadline;
        if (delta <= (uint64_t)INT64_MAX) {
            tick_wall_ms = wall_ms - (int64_t)delta;
        }
        if (!pb_program_tick(program, router, deadline, tick_wall_ms, options)) {
            ok = false;
        }
    }
    return ok;
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
            if (pb_eval_state_deadline(&rule->state.items[state_idx], &deadline) && (!found || deadline < best)) {
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
