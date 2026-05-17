#define _POSIX_C_SOURCE 200809L

#include "pb_soundcheck.h"

#include "fs.h"
#include "pb_eval.h"
#include "pb_json.h"
#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct soundcheck_out {
    bool label;
} soundcheck_out;

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
                         pb_slice subject, pb_slice payload, soundcheck_out *out) {
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
        if (opts.label) {
            printf("in|%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        } else {
            printf("%.*s|%.*s\n", (int)subject.len, subject.ptr, (int)payload.len, payload.ptr);
        }

        for (size_t i = 0; i < parsed.forms.len; i += 1) {
            if (!eval_on_form(parsed.forms.items[i], i, &state, subject, payload, &out)) {
                rc = 1;
            }
        }
    }

    free(line);
    pb_eval_state_free(&state);
    pb_arena_free(&parse_arena);
    mb_buf_free(&source);
    return rc;
}
