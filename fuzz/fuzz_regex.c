// libFuzzer harness for the non-backtracking regex engine (pb_regex.c, pb_regex_dfa.c).
//
// Patterns come from rule files, subjects from untrusted payloads, so both are
// fuzzed. Input format: byte 0 is the pattern length N (clamped to the bytes
// that follow), bytes 1..N are the pattern, the rest is the subject text.
//
// Every compiled pattern is compiled twice, the second copy forced onto the
// Pike VM (a test hook), and the two are compared: the DFA fast path and the
// VM must agree. Searches iterate like re-replace in pb_form_regex.c, sharing
// one budget. A GAVE_UP result is legal; it just ends that comparison.
#include "pb_arena.h"
#include "pb_regex.h"
#include "pb_regex_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            __builtin_trap(); \
        } \
    } while (0)

// Thread-steps per run; small enough that a hard pattern costs milliseconds.
#define FUZZ_BUDGET (256u * 1024)
#define FUZZ_SPANS (PB_REGEX_MAX_GROUPS + 1)

// A reported match must sit inside the text at or after `from`, and every
// participating group must nest inside the whole match.
static void check_spans(const pb_regex_span *sp, size_t groups, size_t from, size_t len) {
    CHECK(sp[0].matched);
    CHECK(sp[0].start >= from && sp[0].start <= sp[0].end && sp[0].end <= len);
    for (size_t g = 1; g <= groups; g += 1) {
        if (sp[g].matched) {
            CHECK(sp[g].start <= sp[g].end && sp[g].end <= len);
            CHECK(sp[g].start >= sp[0].start && sp[g].end <= sp[0].end);
        } else {
            CHECK(sp[g].start == 0 && sp[g].end == 0);
        }
    }
}

static void check_same_spans(const pb_regex_span *a, const pb_regex_span *b, size_t groups) {
    for (size_t g = 0; g <= groups; g += 1) {
        CHECK(a[g].matched == b[g].matched);
        CHECK(a[g].start == b[g].start && a[g].end == b[g].end);
    }
}

static void run_subject(pb_regex *re, pb_regex *pike, pb_slice text) {
    const size_t groups = pb_regex_group_count(re);

    size_t test_budget = FUZZ_BUDGET;
    const pb_regex_result tested = pb_regex_test(re, text, &test_budget);
    CHECK(test_budget <= FUZZ_BUDGET);

    size_t first_budget = FUZZ_BUDGET;
    pb_regex_span first[FUZZ_SPANS];
    memset(first, 0, sizeof first);
    const pb_regex_result searched = pb_regex_search(re, text, 0, first, FUZZ_SPANS, &first_budget);
    CHECK(first_budget <= FUZZ_BUDGET);
    if (searched == PB_REGEX_MATCH) {
        check_spans(first, groups, 0, text.len);
    }
    // test and search ask the same question; either may give up, but not disagree.
    if (tested != PB_REGEX_GAVE_UP && searched != PB_REGEX_GAVE_UP) {
        CHECK(tested == searched);
    }

    // A spans-less search and a one-span search must find the same whole match.
    size_t cap_budget = FUZZ_BUDGET;
    pb_regex_span whole = {0};
    const pb_regex_result one = pb_regex_search(re, text, 0, &whole, 1, &cap_budget);
    if (one != PB_REGEX_GAVE_UP && searched != PB_REGEX_GAVE_UP) {
        CHECK(one == searched);
        if (one == PB_REGEX_MATCH) {
            CHECK(whole.start == first[0].start && whole.end == first[0].end);
        }
    }

    // Unlimited-looking NULL spans / out-of-range from.
    size_t past_budget = FUZZ_BUDGET;
    CHECK(pb_regex_search(re, text, text.len + 1, NULL, 0, &past_budget) == PB_REGEX_NO_MATCH);

    // The DFA-free twin must agree wherever neither side ran out of budget.
    size_t pike_test_budget = FUZZ_BUDGET;
    const pb_regex_result pike_tested = pb_regex_test(pike, text, &pike_test_budget);
    if (tested != PB_REGEX_GAVE_UP && pike_tested != PB_REGEX_GAVE_UP) {
        CHECK(tested == pike_tested);
    }

    // Walk matches like re-replace, sharing one budget per engine.
    size_t budget = FUZZ_BUDGET;
    size_t pike_budget = FUZZ_BUDGET;
    bool compare = true;
    bool any_match = false;
    size_t from = 0;
    while (from <= text.len) {
        pb_regex_span sp[FUZZ_SPANS];
        memset(sp, 0, sizeof sp);
        const size_t before = budget;
        const pb_regex_result found = pb_regex_search(re, text, from, sp, FUZZ_SPANS, &budget);
        CHECK(budget <= before);
        if (compare) {
            pb_regex_span psp[FUZZ_SPANS];
            memset(psp, 0, sizeof psp);
            const pb_regex_result pfound = pb_regex_search(pike, text, from, psp, FUZZ_SPANS, &pike_budget);
            if (found == PB_REGEX_GAVE_UP || pfound == PB_REGEX_GAVE_UP) {
                compare = false;
            } else {
                CHECK(found == pfound);
                if (found == PB_REGEX_MATCH) {
                    check_same_spans(sp, psp, groups);
                }
            }
        }
        if (found != PB_REGEX_MATCH) {
            break;
        }
        check_spans(sp, groups, from, text.len);
        any_match = true;
        from = sp[0].end > sp[0].start ? sp[0].end : sp[0].end + 1;
    }
    // Any match found later in the text implies the whole-text test matches.
    if (any_match && tested != PB_REGEX_GAVE_UP) {
        CHECK(tested == PB_REGEX_MATCH);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) {
        return 0;
    }
    size_t pattern_len = data[0];
    if (pattern_len > size - 1) {
        pattern_len = size - 1;
    }
    const pb_slice pattern = {(const char *)data + 1, pattern_len};
    const pb_slice text = {(const char *)data + 1 + pattern_len, size - 1 - pattern_len};

    pb_arena arena = {0};
    pb_regex *re = NULL;
    size_t err_offset = 0;
    char err_msg[96];
    memset(err_msg, 0, sizeof err_msg);
    const pb_regex_error err = pb_regex_compile(&arena, pattern, &re, &err_offset, err_msg, sizeof err_msg);
    if (err != PB_REGEX_OK) {
        CHECK(re == NULL);
        CHECK(err_offset <= pattern.len);
        CHECK(memchr(err_msg, '\0', sizeof err_msg) != NULL);
        CHECK(pb_regex_error_name(err) != NULL);
        pb_arena_free(&arena);
        return 0;
    }
    CHECK(re != NULL);
    CHECK(pb_regex_group_count(re) <= PB_REGEX_MAX_GROUPS);
    CHECK(pb_regex_program_len(re) <= PB_REGEX_MAX_PROGRAM);
    const pb_slice source = pb_regex_source(re);
    CHECK(source.len == pattern.len && (pattern.len == 0 || memcmp(source.ptr, pattern.ptr, pattern.len) == 0));
    CHECK(pb_regex_has_dfa(re) == (pb_regex_dfa_states(re) != 0));

    pb_regex *pike = NULL;
    if (pb_regex_compile(&arena, pattern, &pike, NULL, NULL, 0) == PB_REGEX_OK) {
        // Same pattern, same arena: must compile again, and must have the same shape.
        CHECK(pb_regex_group_count(pike) == pb_regex_group_count(re));
        CHECK(pb_regex_program_len(pike) == pb_regex_program_len(re));
        pb_regex_disable_dfa(pike);
        CHECK(!pb_regex_has_dfa(pike));
        run_subject(re, pike, text);
    }
    pb_arena_free(&arena);
    return 0;
}
