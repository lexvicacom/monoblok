#define _POSIX_C_SOURCE 200809L

#include "pb_soundcheck.h"

#include "array.h"
#include "fs.h"
#include "pb_config.h"
#include "pb_eval_internal.h"
#include "pb_json.h"
#include "pb_program.h"
#include "router.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// One last-value row owned by soundcheck_out; heap bytes freed at cleanup.
typedef struct soundcheck_last {
    char *subject;
    size_t subject_len;
    char *payload;
    size_t payload_len;
} soundcheck_last;

// Output options, the rule table, and the stand-in LVC for `(last ...)`.
// forms borrows the parse arena; reentrant[i] is the `:reentrant` flag of
// form i (heap-owned). filters borrow the parse arena; rows are heap-owned and
// updated from input and emitted rows.
typedef struct soundcheck_out {
    bool label;
    pb_values forms;
    bool *reentrant;
    pb_eval_state *state;
    pb_lvc_config lvc;
    soundcheck_last *rows;
    size_t rows_len;
    size_t rows_cap;
} soundcheck_out;

static uint64_t monotonic_ms(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * UINT64_C(1000) + (uint64_t)(ts.tv_nsec / 1000000);
}

static int64_t wall_clock_ms(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * INT64_C(1000) + (int64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(uint64_t ms) {
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000),
        .tv_nsec = (long)((ms % 1000) * 1000000),
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

static bool token_match(pb_slice filter, pb_slice subject) {
    return mb_router_subject_matches((mb_slice){.ptr = (const uint8_t *)filter.ptr, .len = filter.len},
                                     (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len});
}

static bool last_covered(const soundcheck_out *out, pb_slice subject) {
    for (size_t i = 0; i < out->lvc.len; i += 1) {
        if (token_match(out->lvc.filters[i], subject)) {
            return true;
        }
    }
    return false;
}

// Mirrors the router LVC: only subjects covered by an (lvc ...) filter are kept.
static bool last_store(soundcheck_out *out, pb_slice subject, pb_slice payload) {
    if (!last_covered(out, subject)) {
        return true;
    }
    soundcheck_last *row = NULL;
    for (size_t i = 0; i < out->rows_len; i += 1) {
        if (pb_slice_eq((pb_slice){.ptr = out->rows[i].subject, .len = out->rows[i].subject_len}, subject)) {
            row = &out->rows[i];
            break;
        }
    }
    if (row == NULL) {
        if (!mb_array_reserve((void **)&out->rows, &out->rows_cap, out->rows_len + 1, sizeof out->rows[0], 8)) {
            return false;
        }
        row = &out->rows[out->rows_len];
        *row = (soundcheck_last){0};
        row->subject = malloc(subject.len == 0 ? 1 : subject.len);
        if (row->subject == NULL) {
            return false;
        }
        memcpy(row->subject, subject.ptr, subject.len);
        row->subject_len = subject.len;
        out->rows_len += 1;
    }
    char *payload_copy = malloc(payload.len == 0 ? 1 : payload.len);
    if (payload_copy == NULL) {
        return false;
    }
    memcpy(payload_copy, payload.ptr, payload.len);
    free(row->payload);
    row->payload = payload_copy;
    row->payload_len = payload.len;
    return true;
}

static void last_free(soundcheck_out *out) {
    for (size_t i = 0; i < out->rows_len; i += 1) {
        free(out->rows[i].subject);
        free(out->rows[i].payload);
    }
    free(out->rows);
    free(out->lvc.filters);
    free(out->reentrant);
    *out = (soundcheck_out){0};
}

static bool last_cb(void *ctx, pb_slice subject, pb_slice *payload) {
    const soundcheck_out *out = ctx;
    for (size_t i = 0; i < out->rows_len; i += 1) {
        if (pb_slice_eq((pb_slice){.ptr = out->rows[i].subject, .len = out->rows[i].subject_len}, subject)) {
            *payload = (pb_slice){.ptr = out->rows[i].payload, .len = out->rows[i].payload_len};
            return true;
        }
    }
    return false;
}

// Publish hook context for one rule evaluation or clock tick; stack-owned.
// Mirrors publish_ctx in pb_program.c: depth counts re-entrant hops.
typedef struct soundcheck_pub {
    soundcheck_out *out;
    size_t rule_idx;
    size_t depth;
    uint64_t now_ms;
    int64_t wall_ms;
} soundcheck_pub;

static bool dispatch_publish(soundcheck_out *out, pb_slice subject, pb_slice payload,
                             uint64_t now_ms, int64_t wall_ms, size_t depth);

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    const soundcheck_pub *pub = ctx;
    soundcheck_out *out = pub->out;
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject)) {
        return false;
    }
    if (!last_store(out, subject, payload)) {
        return false;
    }
    if (out->label) {
        printf("out|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    } else {
        printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    }
    if (!out->reentrant[pub->rule_idx]) {
        return true;
    }
    if (pub->depth >= PB_PROGRAM_MAX_REENTRY_DEPTH) {
        fprintf(stderr, "soundcheck: reentry depth cap reached\n");
        return true;
    }
    // Same order as the server: the emitted row is already printed, then its re-evaluation.
    (void)dispatch_publish(out, subject, payload, pub->now_ms, pub->wall_ms, pub->depth + 1);
    return true;
}

static bool eval_on_form(pb_value form, size_t rule_id, pb_eval_state *state,
                         pb_slice subject, pb_slice payload, uint64_t now_ms,
                         int64_t wall_ms, size_t depth, soundcheck_out *out) {
    if (form.kind != PB_LIST || form.seq.len < 3) {
        return true;
    }
    pb_values items = form.seq;
    if (items.items[0].kind != PB_SYMBOL || !pb_slice_eq_lit(items.items[0].text, "on")) {
        return true;
    }
    if (items.items[1].kind != PB_STRING) {
        fprintf(stderr, "soundcheck: invalid on form\n");
        return false;
    }
    if (!token_match(items.items[1].text, subject)) {
        return true;
    }

    pb_arena scratch = {0};
    soundcheck_pub pub = {.out = out, .rule_idx = rule_id, .depth = depth, .now_ms = now_ms, .wall_ms = wall_ms};
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = state,
        .rule_id = rule_id,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .subject = subject,
        .payload = payload,
        .publish = publish_cb,
        .publish_ctx = &pub,
        .last = last_cb,
        .last_ctx = out,
    };
    const pb_eval_result r = pb_eval(&ctx, items.items[items.len - 1]);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "soundcheck: eval error: %s\n", pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

// Offers a published message to every on rule in file order, like
// eval_publish_slices in pb_program.c. Re-entrant emissions call back into it.
static bool dispatch_publish(soundcheck_out *out, pb_slice subject, pb_slice payload,
                             uint64_t now_ms, int64_t wall_ms, size_t depth) {
    bool ok = true;
    for (size_t i = 0; i < out->forms.len; i += 1) {
        if (!eval_on_form(out->forms.items[i], i, out->state, subject, payload, now_ms, wall_ms, depth, out)) {
            ok = false;
        }
    }
    return ok;
}

static bool next_deadline(const pb_eval_state *state, uint64_t *out_ms) {
    bool found = false;
    uint64_t best = 0;
    for (size_t i = 0; i < state->len; i += 1) {
        uint64_t deadline = 0;
        if (pb_eval_state_deadline(&state->items[i], &deadline) && (!found || deadline < best)) {
            best = deadline;
            found = true;
        }
    }
    if (found) {
        *out_ms = best;
    }
    return found;
}

static bool tick_due_once(pb_eval_state *state, uint64_t now_ms, int64_t wall_ms, soundcheck_out *out) {
    pb_arena scratch = {0};
    bool ok = true;
    for (size_t i = 0; i < state->len; i += 1) {
        pb_eval_state_entry *entry = &state->items[i];
        uint64_t deadline = 0;
        if (!pb_eval_state_deadline(entry, &deadline) || deadline > now_ms) {
            continue;
        }
        soundcheck_pub pub = {.out = out, .rule_idx = entry->rule_id, .depth = 0, .now_ms = now_ms, .wall_ms = wall_ms};
        pb_eval_ctx ctx = {
            .arena = &scratch,
            .state = state,
            .rule_id = entry->rule_id,
            .now_ms = now_ms,
            .wall_ms = wall_ms,
            .subject = (pb_slice){.ptr = entry->subject, .len = entry->subject_len},
            .payload = {.ptr = "", .len = 0},
            .publish = publish_cb,
            .publish_ctx = &pub,
            .last = last_cb,
            .last_ctx = out,
        };
        const pb_eval_result r = pb_eval_tick_state_entry(&ctx, entry);
        if (r.err != PB_EVAL_OK) {
            fprintf(stderr, "soundcheck: clock tick error: %s\n", pb_eval_error_name(r.err));
            ok = false;
        }
    }
    pb_arena_free(&scratch);
    return ok;
}

static bool tick_due(pb_eval_state *state, uint64_t now_ms, soundcheck_out *out) {
    bool ok = true;
    for (;;) {
        uint64_t deadline = 0;
        if (!next_deadline(state, &deadline) || deadline > now_ms) {
            break;
        }
        if (!tick_due_once(state, now_ms, wall_clock_ms(), out)) {
            ok = false;
        }
    }
    return ok;
}

static bool linger(pb_eval_state *state, uint64_t linger_ms, soundcheck_out *out) {
    if (linger_ms == 0) {
        return true;
    }
    const uint64_t start_ms = monotonic_ms();
    const uint64_t end_ms = UINT64_MAX - start_ms < linger_ms ? UINT64_MAX : start_ms + linger_ms;
    bool ok = true;

    for (;;) {
        uint64_t deadline = 0;
        if (!next_deadline(state, &deadline) || deadline > end_ms) {
            break;
        }
        uint64_t now_ms = monotonic_ms();
        if (deadline > now_ms) {
            sleep_ms(deadline - now_ms);
            now_ms = monotonic_ms();
        }
        if (now_ms > end_ms) {
            now_ms = end_ms;
        }
        if (!tick_due(state, now_ms, out)) {
            ok = false;
        }
    }
    return ok;
}

int pb_soundcheck_run(const char *path, pb_soundcheck_options opts) {
    int rc = 1;
    mb_buf source = {0};
    pb_arena parse_arena = {0};
    pb_eval_state state = {0};
    soundcheck_out out = {.label = opts.label};
    pb_bridge_config bridge_unused = {0};
    pb_imports_config imports_unused = {0};
    char *line = NULL;
    if (!mb_read_file(path, &source)) {
        perror(path);
        goto cleanup;
    }

    const pb_parse_result parsed = pb_parse_patchbay_source(&parse_arena, path, (const char *)source.ptr, source.len);
    if (parsed.err != PB_PARSE_OK) {
        size_t err_line = 0;
        size_t err_col = 0;
        pb_line_col((const char *)source.ptr, source.len, parsed.err_offset, &err_line, &err_col);
        fprintf(stderr, "soundcheck: parse error: %s%s%s at %zu:%zu (byte %zu)\n",
                pb_parse_error_name(parsed.err), parsed.err_msg[0] != '\0' ? ": " : "", parsed.err_msg, err_line, err_col, parsed.err_offset);
        goto cleanup;
    }

    out.forms = parsed.forms;
    out.state = &state;
    out.reentrant = calloc(parsed.forms.len == 0 ? 1 : parsed.forms.len, sizeof out.reentrant[0]);
    if (out.reentrant == NULL) {
        fprintf(stderr, "soundcheck: out of memory\n");
        goto cleanup;
    }

    // Same resolve pass as normal startup: tags call sites, rejects unknown forms.
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        pb_value form = parsed.forms.items[i];
        if (form.kind == PB_LIST && form.seq.len >= 3 && form.seq.items[0].kind == PB_SYMBOL &&
            pb_slice_eq_lit(form.seq.items[0].text, "on")) {
            char err[160];
            // Options sit between the filter and the body, as in load_on_form().
            for (size_t o = 2; o < form.seq.len - 1; o += 2) {
                const pb_value key = form.seq.items[o];
                const pb_value val = form.seq.items[o + 1];
                if (o + 1 >= form.seq.len - 1 || key.kind != PB_KEYWORD || !pb_slice_eq_lit(key.text, "reentrant") || val.kind != PB_BOOL) {
                    fprintf(stderr, "soundcheck: rule %zu: invalid on options (expected :reentrant BOOL)\n", i);
                    goto cleanup;
                }
                out.reentrant[i] = val.boolean;
            }
            if (!pb_resolve_rule(&parse_arena, &parsed.forms.items[i].seq.items[form.seq.len - 1], false, false, err, sizeof err)) {
                fprintf(stderr, "soundcheck: rule %zu: %s\n", i, err);
                goto cleanup;
            }
        }
        if (form.kind == PB_LIST && form.seq.len != 0 && form.seq.items[0].kind == PB_SYMBOL &&
            pb_slice_eq_lit(form.seq.items[0].text, "every")) {
            // The slot is scheduled by the first tick (now + ms) and fires during input or linger.
            pb_every_spec spec = {0};
            char err[160];
            if (!pb_every_parse(&parse_arena, form.seq, false, false, &spec, err, sizeof err)) {
                fprintf(stderr, "soundcheck: rule %zu (every): %s\n", i, err);
                goto cleanup;
            }
            out.reentrant[i] = spec.reentrant;
            if (!pb_eval_every_arm(&state, i, spec.ms, spec.body)) {
                fprintf(stderr, "soundcheck: out of memory\n");
                goto cleanup;
            }
        }
    }

    {
        // Only (lvc ...) forms are read; they decide what `last` can see.
        const pb_config_ctx config_ctx = {.arena = &parse_arena, .prefix = "soundcheck"};
        for (size_t i = 0; i < parsed.forms.len; i += 1) {
            const pb_value form = parsed.forms.items[i];
            if (form.kind == PB_LIST && form.seq.len != 0 && form.seq.items[0].kind == PB_SYMBOL &&
                pb_slice_eq_lit(form.seq.items[0].text, "lvc") &&
                !pb_config_load_form(&config_ctx, form.seq, &out.lvc, &bridge_unused, &imports_unused)) {
                goto cleanup;
            }
        }
    }
    size_t cap = 0;
    ssize_t n = 0;
    rc = 0;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            n -= 1;
        }
        if (n == 0) {
            continue;
        }
        char *sep = memchr(line, '|', (size_t)n);
        if (sep == NULL) {
            fprintf(stderr, "soundcheck: expected SUBJECT|payload, got: %.*s\n", (int)n, line);
            rc = 1;
            continue;
        }

        pb_slice subject = {.ptr = line, .len = (size_t)(sep - line)};
        pb_slice payload = {.ptr = sep + 1, .len = (size_t)(line + n - (sep + 1))};
        const uint64_t now_ms = monotonic_ms();
        const int64_t wall_ms = wall_clock_ms();
        if (opts.label) {
            printf("in|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        } else {
            printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        }
        // A client publish reaches the LVC before its rules run.
        if (!last_store(&out, subject, payload)) {
            rc = 1;
        }

        if (!dispatch_publish(&out, subject, payload, now_ms, wall_ms, 0)) {
            rc = 1;
        }
        if (!tick_due(&state, now_ms, &out)) {
            rc = 1;
        }
    }

    if (!linger(&state, opts.linger_ms, &out)) {
        rc = 1;
    }

cleanup:
    last_free(&out);
    free(line);
    pb_eval_state_free(&state);
    pb_arena_free(&parse_arena);
    mb_buf_free(&source);
    return rc;
}
