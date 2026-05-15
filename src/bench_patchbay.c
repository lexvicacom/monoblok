#define _POSIX_C_SOURCE 200809L

#include "buf.h"
#include "fs.h"
#include "pb_program.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum bench_mode {
    BENCH_PASS,
    BENCH_GATE,
    BENCH_WINDOW,
    BENCH_JSON,
    BENCH_MIXED,
} bench_mode;

typedef struct noop_publisher {
    uint64_t count;
} noop_publisher;

enum { BENCH_SCRATCH_RETAIN_BYTES = 4 * 1024 * 1024 };

static const char *mode_name(bench_mode mode) {
    switch (mode) {
    case BENCH_PASS: return "pass";
    case BENCH_GATE: return "gate";
    case BENCH_WINDOW: return "window";
    case BENCH_JSON: return "json";
    case BENCH_MIXED: return "mixed";
    }
    return "?";
}

static bool parse_mode(const char *s, bench_mode *out) {
    if (strcmp(s, "pass") == 0) *out = BENCH_PASS;
    else if (strcmp(s, "gate") == 0) *out = BENCH_GATE;
    else if (strcmp(s, "window") == 0) *out = BENCH_WINDOW;
    else if (strcmp(s, "json") == 0) *out = BENCH_JSON;
    else if (strcmp(s, "mixed") == 0) *out = BENCH_MIXED;
    else return false;
    return true;
}

static bool publish_noop(void *ctx, pb_slice subject, pb_slice payload) {
    (void)subject;
    (void)payload;
    noop_publisher *pub = ctx;
    pub->count += 1;
    return true;
}

static uint64_t mono_ns(void) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool append_line(mb_buf *buf, const char *fmt, size_t i) {
    char tmp[256];
    const int n = snprintf(tmp, sizeof tmp, fmt, i);
    if (n < 0 || (size_t)n >= sizeof tmp) {
        return false;
    }
    return mb_buf_append(buf, tmp, (size_t)n);
}

static bool build_source(bench_mode mode, size_t n, char **out, size_t *out_len) {
    mb_buf buf = {0};
    for (size_t i = 0; i < n; i += 1) {
        bool ok = false;
        switch (mode) {
        case BENCH_PASS:
            ok = append_line(&buf,
                             "(on \"sensors.*\" (publish! (subject-append \"r%zu\") payload))\n",
                             i);
            break;
        case BENCH_GATE:
            ok = append_line(&buf,
                             "(on \"sensors.*\" (-> payload-float (squelch) (publish! (subject-append \"r%zu\"))))\n",
                             i);
            break;
        case BENCH_WINDOW:
            ok = append_line(&buf,
                             "(on \"sensors.*\" (-> payload-float (moving-avg 10) (publish! (subject-append \"r%zu\"))))\n",
                             i);
            break;
        case BENCH_JSON:
            ok = mb_buf_append(&buf,
                               "(on \"sensors.*\" (json-demux! \"a\" \"b\" \"c\" payload))\n",
                               strlen("(on \"sensors.*\" (json-demux! \"a\" \"b\" \"c\" payload))\n"));
            break;
        case BENCH_MIXED:
            break;
        }
        if (!ok) {
            mb_buf_free(&buf);
            return false;
        }
    }
    if (!mb_buf_append_byte(&buf, '\0')) {
        mb_buf_free(&buf);
        return false;
    }
    *out = (char *)buf.ptr;
    *out_len = buf.len - 1;
    return true;
}

static bool subject_matches(pb_slice filter, pb_slice subject) {
    return mb_router_subject_matches((mb_slice){.ptr = (const uint8_t *)filter.ptr, .len = filter.len},
                                     (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len});
}

static bool run_program(pb_program *program, pb_slice subject, pb_slice payload,
                        uint64_t now_ms, noop_publisher *pub) {
    bool ok = true;
    for (size_t i = 0; i < program->len; i += 1) {
        pb_rule *rule = &program->rules[i];
        if (!subject_matches(rule->filter, subject)) {
            continue;
        }
        pb_eval_ctx ctx = {
            .arena = &program->scratch,
            .state = &rule->state,
            .rule_id = i,
            .now_ms = now_ms,
            .wall_ms = 0,
            .subject = subject,
            .payload = payload,
            .publish = publish_noop,
            .publish_ctx = pub,
        };
        const pb_eval_result r = pb_eval(&ctx, rule->body);
        if (r.err != PB_EVAL_OK) {
            fprintf(stderr, "patchbay bench: rule %zu failed: %s\n", i, pb_eval_error_name(r.err));
            ok = false;
        }
    }
    pb_arena_reset(&program->scratch);
    pb_arena_trim(&program->scratch, BENCH_SCRATCH_RETAIN_BYTES);
    return ok;
}

static pb_slice pick_payload(bench_mode mode, size_t i) {
    static const char *floats[] = {
        "1.0", "1.1", "1.2", "1.3", "1.4", "1.5", "1.6", "1.7",
        "2.0", "2.1", "2.2", "2.3", "2.4", "2.5", "2.6", "2.7",
    };
    const char *s = mode == BENCH_JSON ? "{\"a\":1,\"b\":2,\"c\":3}" : floats[i % (sizeof floats / sizeof floats[0])];
    return (pb_slice){.ptr = s, .len = strlen(s)};
}

static bool parse_size(const char *s, size_t *out) {
    char *end = NULL;
    const unsigned long long v = strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (size_t)v;
    return true;
}

int main(int argc, char **argv) {
    bench_mode mode = BENCH_PASS;
    if (argc > 1 && !parse_mode(argv[1], &mode)) {
        fprintf(stderr, "bad mode '%s' (want pass|gate|window|json|mixed)\n", argv[1]);
        return 2;
    }

    size_t n = 1;
    size_t pubs = 1000000;
    if (argc > 2 && !parse_size(argv[2], &n)) {
        fprintf(stderr, "bad N '%s'\n", argv[2]);
        return 2;
    }
    if (argc > 3 && !parse_size(argv[3], &pubs)) {
        fprintf(stderr, "bad PUBS '%s'\n", argv[3]);
        return 2;
    }
    if (mode != BENCH_MIXED && n == 0) {
        fprintf(stderr, "N must be > 0\n");
        return 2;
    }

    char *source = NULL;
    size_t source_len = 0;
    mb_buf file_source = {0};
    bool source_ok = false;
    if (mode == BENCH_MIXED) {
        source_ok = mb_read_file("patchbay.edn", &file_source);
        if (!source_ok) {
            perror("patchbay.edn");
        } else {
            source = (char *)file_source.ptr;
            source_len = file_source.len;
        }
    } else {
        source_ok = build_source(mode, n, &source, &source_len);
    }
    if (!source_ok) {
        return 1;
    }

    pb_program program = {0};
    if (!pb_program_load_source(&program, mode == BENCH_MIXED ? "patchbay.edn" : "<bench>", source, source_len)) {
        if (mode == BENCH_MIXED) {
            mb_buf_free(&file_source);
        } else {
            free(source);
        }
        return 1;
    }
    if (mode == BENCH_MIXED) {
        mb_buf_free(&file_source);
    } else {
        free(source);
    }

    noop_publisher pub = {0};
    char subject_buf[64];
    for (size_t i = 0; i < 10000; i += 1) {
        const int nsubj = snprintf(subject_buf, sizeof subject_buf, "sensors.k%zu", i % 1024);
        if (nsubj < 0 || (size_t)nsubj >= sizeof subject_buf) {
            pb_program_free(&program);
            return 1;
        }
        const pb_slice subject = {.ptr = subject_buf, .len = (size_t)nsubj};
        if (!run_program(&program, subject, pick_payload(mode, i), i, &pub)) {
            pb_program_free(&program);
            return 1;
        }
    }
    const uint64_t warm_emits = pub.count;
    pub.count = 0;

    const uint64_t start = mono_ns();
    for (size_t i = 0; i < pubs; i += 1) {
        const int nsubj = snprintf(subject_buf, sizeof subject_buf, "sensors.k%zu", i % 1024);
        if (nsubj < 0 || (size_t)nsubj >= sizeof subject_buf) {
            pb_program_free(&program);
            return 1;
        }
        const pb_slice subject = {.ptr = subject_buf, .len = (size_t)nsubj};
        if (!run_program(&program, subject, pick_payload(mode, i), i + 10000, &pub)) {
            pb_program_free(&program);
            return 1;
        }
    }
    const uint64_t elapsed_ns = mono_ns() - start;

    const double elapsed_s = (double)elapsed_ns / 1000000000.0;
    const double rate = (double)pubs / elapsed_s;
    const double ns_per_pub = (double)elapsed_ns / (double)pubs;
    const double emits_per_pub = (double)pub.count / (double)pubs;

    printf("mode=%s rules=%zu | %zu pubs in %.3fs = %.0f pubs/s (%.1f ns/pub, %.2f emits/pub, warm_emits=%" PRIu64 ")\n",
           mode_name(mode), program.len, pubs, elapsed_s, rate, ns_per_pub, emits_per_pub, warm_emits);

    pb_program_free(&program);
    return 0;
}
