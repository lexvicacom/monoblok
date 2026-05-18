#define _POSIX_C_SOURCE 200809L

#include "pb_soundcheck.h"

#include "fs.h"
#include "pb_eval.h"
#include "pb_json.h"
#include "router.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct soundcheck_out {
    bool label;
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

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    soundcheck_out *out = ctx;
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject)) {
        return false;
    }
    if (out->label) {
        printf("out|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    } else {
        printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
    }
    return true;
}

static bool eval_on_form(pb_value form, size_t rule_id, pb_eval_state *state,
                         pb_slice subject, pb_slice payload, uint64_t now_ms,
                         int64_t wall_ms, soundcheck_out *out) {
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
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = state,
        .rule_id = rule_id,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .subject = subject,
        .payload = payload,
        .publish = publish_cb,
        .publish_ctx = out,
    };
    const pb_eval_result r = pb_eval(&ctx, items.items[items.len - 1]);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "soundcheck: eval error: %s\n", pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

static bool state_entry_deadline(const pb_eval_state_entry *entry, uint64_t *out_ms) {
    if (entry->kind == PB_EVAL_STATE_RING && entry->ring_time_window && entry->ring_len > 0) {
        const size_t idx = entry->ring_start;
        *out_ms = entry->ring_times_ms[idx] + entry->ring_window_ms;
        return true;
    }
    if (entry->kind == PB_EVAL_STATE_BAR && entry->bar_time_window && entry->bar_count > 0) {
        *out_ms = entry->bar_window_start_ms + entry->bar_window_ms;
        return true;
    }
    if (entry->kind == PB_EVAL_STATE_CLOCK && entry->clock_armed) {
        *out_ms = entry->clock_deadline_ms;
        return true;
    }
    return false;
}

static bool next_deadline(const pb_eval_state *state, uint64_t *out_ms) {
    bool found = false;
    uint64_t best = 0;
    for (size_t i = 0; i < state->len; i += 1) {
        uint64_t deadline = 0;
        if (state_entry_deadline(&state->items[i], &deadline) && (!found || deadline < best)) {
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
        if (!state_entry_deadline(entry, &deadline) || deadline > now_ms) {
            continue;
        }
        pb_eval_ctx ctx = {
            .arena = &scratch,
            .state = state,
            .rule_id = entry->rule_id,
            .now_ms = now_ms,
            .wall_ms = wall_ms,
            .subject = {.ptr = entry->subject, .len = entry->subject_len},
            .payload = {.ptr = "", .len = 0},
            .publish = publish_cb,
            .publish_ctx = out,
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
    mb_buf source = {0};
    if (!mb_read_file(path, &source)) {
        perror(path);
        return 1;
    }

    pb_arena parse_arena = {0};
    const pb_parse_result parsed = pb_parse_patchbay_source(&parse_arena, path, (const char *)source.ptr, source.len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "soundcheck: parse error: %s at byte %zu\n",
                pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_arena_free(&parse_arena);
        mb_buf_free(&source);
        return 1;
    }

    soundcheck_out out = {.label = opts.label};
    pb_eval_state state = {0};
    char *line = NULL;
    size_t cap = 0;
    ssize_t n = 0;
    int rc = 0;
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

        for (size_t i = 0; i < parsed.forms.len; i += 1) {
            if (!eval_on_form(parsed.forms.items[i], i, &state, subject, payload, now_ms, wall_ms, &out)) {
                rc = 1;
            }
        }
        if (!tick_due(&state, now_ms, &out)) {
            rc = 1;
        }
    }

    if (!linger(&state, opts.linger_ms, &out)) {
        rc = 1;
    }

    free(line);
    pb_eval_state_free(&state);
    pb_arena_free(&parse_arena);
    mb_buf_free(&source);
    return rc;
}
