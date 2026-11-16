// Shared body of the patchbay rule-file harnesses (EDN, JSON, YAML).
//
// The input is copied into an exact-size malloc'd buffer so ASan catches reads
// past `size`; the loaders accept non-NUL-terminated (ptr, len) sources. The
// source label picks the adapter in pb_parse_patchbay_source (".json", ".yaml"
// by suffix; anything else is EDN, or JSON when the text starts with [ or {).
//
// After a successful parse the harness goes through pb_program_load_source
// (resolve, validate, config load, rule index), then evaluates a few publishes
// and clock ticks against an in-memory router. There is no file, socket, or
// timer I/O. The loader logs diagnostics to stderr; that is muted per call by
// swapping the stderr FILE* (ASan and libFuzzer report through fd 2 directly).
#ifndef FUZZ_PATCHBAY_COMMON_H
#define FUZZ_PATCHBAY_COMMON_H

#include "pb_program.h"
#include "router.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    FUZZ_PB_MAX_RULES_EVALUATED = 8,
    FUZZ_PB_SUBJECT_CAP = 128,
};

static mb_slice fuzz_pb_lit(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

// Turn a rule filter into a concrete subject: wildcard tokens become "x".
static size_t fuzz_pb_concrete_subject(pb_slice filter, char *out, size_t cap) {
    size_t n = 0;
    size_t i = 0;
    while (i < filter.len && n + 2 < cap) {
        const char c = filter.ptr[i];
        const bool token_start = i == 0 || filter.ptr[i - 1] == '.';
        const bool token_end = i + 1 == filter.len || filter.ptr[i + 1] == '.';
        if (token_start && token_end && (c == '*' || c == '>')) {
            out[n++] = 'x';
        } else {
            out[n++] = c;
        }
        i += 1;
    }
    if (n == 0) {
        out[n++] = 'x';
    }
    out[n] = '\0';
    return n;
}

static void fuzz_pb_exercise_program(pb_program *program) {
    mb_router router;
    mb_router_init(&router);
    mb_router_conn conn = {0};
    (void)mb_router_subscribe(&router, &conn, fuzz_pb_lit(">"), fuzz_pb_lit("1"));

    static const char *const payloads[] = {"42", "-3.5", "{\"a\":1,\"b\":{\"c\":\"x\"}}", "hello world", ""};
    const size_t payload_count = sizeof payloads / sizeof payloads[0];
    uint64_t now_ms = 1000;
    const size_t rules = program->len < FUZZ_PB_MAX_RULES_EVALUATED ? program->len : FUZZ_PB_MAX_RULES_EVALUATED;
    for (size_t i = 0; i < rules; i += 1) {
        char subject[FUZZ_PB_SUBJECT_CAP];
        const size_t subject_len = fuzz_pb_concrete_subject(program->rules[i].filter, subject, sizeof subject);
        for (size_t p = 0; p < payload_count; p += 1) {
            const mb_slice payload = fuzz_pb_lit(payloads[p]);
            (void)pb_program_eval_publish(program, &router,
                                          (mb_slice){.ptr = (const uint8_t *)subject, .len = subject_len}, payload,
                                          now_ms, (int64_t)now_ms, (pb_program_eval_options){0});
            now_ms += 250;
            conn.out.len = 0;
        }
    }
    uint64_t deadline = 0;
    (void)pb_program_next_clock_deadline(program, &deadline);
    (void)pb_program_tick(program, &router, now_ms + 1000, (int64_t)now_ms + 1000, (pb_program_eval_options){0});
    (void)pb_program_tick_until(program, &router, now_ms + 5000, (int64_t)now_ms + 5000,
                                (pb_program_eval_options){0});

    mb_buf_free(&conn.out);
    mb_router_free(&router);
}

static int fuzz_pb_run(const char *label, const uint8_t *data, size_t size) {
    char *src = malloc(size == 0 ? 1 : size);
    if (src == NULL) {
        return 0;
    }
    if (size != 0) {
        memcpy(src, data, size);
    }

    FILE *real_stderr = stderr;
    FILE *quiet = fopen("/dev/null", "w");
    if (quiet != NULL) {
        stderr = quiet;
    }

    pb_program program = {0};
    if (pb_program_load_source(&program, label, src, size)) {
        fuzz_pb_exercise_program(&program);
        pb_program_free(&program);
    }

    stderr = real_stderr;
    if (quiet != NULL) {
        fclose(quiet);
    }
    free(src);
    return 0;
}

#endif
