#include "pb_regex.h"
#include "pb_regex_internal.h"
#include "test_check.h"

#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pb_slice sl(const char *s) {
    return (pb_slice){s, strlen(s)};
}

// Unlimited-budget wrappers: most tests only care about the answer.
static bool t_test(pb_regex *re, pb_slice text) {
    pb_regex_result r = pb_regex_test(re, text, NULL);
    CHECK(r != PB_REGEX_GAVE_UP);
    return r == PB_REGEX_MATCH;
}

static bool t_search(pb_regex *re, pb_slice text, size_t from, pb_regex_span *spans, size_t spans_cap) {
    pb_regex_result r = pb_regex_search(re, text, from, spans, spans_cap, NULL);
    CHECK(r != PB_REGEX_GAVE_UP);
    return r == PB_REGEX_MATCH;
}

// The suite runs twice: DFA fast path, then forced onto the Pike VM.
static bool g_force_pike;

static pb_regex *compile_ok(pb_arena *arena, pb_slice pat) {
    pb_regex *re = NULL;
    size_t off = 0;
    char msg[96];
    pb_regex_error err = pb_regex_compile(arena, pat, &re, &off, msg, sizeof msg);
    if (err != PB_REGEX_OK) {
        fprintf(stderr, "unexpected compile error for %.*s: %s at %zu (%s)\n", (int)pat.len, pat.ptr, pb_regex_error_name(err), off, msg);
    }
    CHECK(err == PB_REGEX_OK);
    CHECK(re != NULL);
    if (g_force_pike) pb_regex_disable_dfa(re);
    return re;
}

// Table row: ms < 0 means no match. g lists expected group spans for groups 1..ng ({-1,-1} = did not participate).
typedef struct rx_case {
    const char *pat;
    const char *text;
    int ms;
    int me;
    int ng;
    int g[4][2];
} rx_case;

static void run_case(const rx_case *tc) {
    pb_arena arena = {0};
    pb_regex *re = compile_ok(&arena, sl(tc->pat));
    pb_slice text = sl(tc->text);
    pb_regex_span spans[10];
    memset(spans, 0xAB, sizeof spans);
    bool hit = t_search(re, text, 0, spans, 10);
    bool ok = hit == (tc->ms >= 0) && t_test(re, text) == hit;
    if (ok && hit) {
        ok = spans[0].matched && (int)spans[0].start == tc->ms && (int)spans[0].end == tc->me;
        for (int i = 0; ok && i < tc->ng; i += 1) {
            const pb_regex_span *s = &spans[i + 1];
            if (tc->g[i][0] < 0) {
                ok = !s->matched;
            } else {
                ok = s->matched && (int)s->start == tc->g[i][0] && (int)s->end == tc->g[i][1];
            }
        }
    }
    if (!ok) {
        fprintf(stderr, "table case failed: /%s/ on \"%s\" hit=%d span=%zu..%zu\n", tc->pat, tc->text, hit, spans[0].start, spans[0].end);
    }
    CHECK(ok);
    pb_arena_free(&arena);
}

static void test_table(void) {
    static const rx_case cases[] = {
        // literals, escapes, dot
        {"abc", "xxabcxx", 2, 5, 0, {{0}}},
        {"abc", "ab", -1, 0, 0, {{0}}},
        {"a.c", "a\nc abc", 4, 7, 0, {{0}}},
        {"(?s)a.c", "a\nc", 0, 3, 0, {{0}}},
        {"\\.", "a.b", 1, 2, 0, {{0}}},
        {"a\\.b", "axb", -1, 0, 0, {{0}}},
        {"\\\\", "a\\b", 1, 2, 0, {{0}}},
        {"\\[x\\]", "a[x]", 1, 4, 0, {{0}}},
        {"\\n", "a\nb", 1, 2, 0, {{0}}},
        {"\\r\\t", "a\r\tb", 1, 3, 0, {{0}}},
        {"\\x41\\x62", "xAb", 1, 3, 0, {{0}}},
        {"a\\-b", "a-b", 0, 3, 0, {{0}}},
        {"a}b]", "a}b]", 0, 4, 0, {{0}}},
        // quantifiers, greedy and lazy
        {"a*", "baaa", 0, 0, 0, {{0}}},
        {"a+", "baaa", 1, 4, 0, {{0}}},
        {"a+?", "baaa", 1, 2, 0, {{0}}},
        {"a*?b", "aab", 0, 3, 0, {{0}}},
        {"a?", "a", 0, 1, 0, {{0}}},
        {"a??", "a", 0, 0, 0, {{0}}},
        {"a{2}", "aaa", 0, 2, 0, {{0}}},
        {"a{2,}", "aaaa", 0, 4, 0, {{0}}},
        {"a{1,2}", "aaa", 0, 2, 0, {{0}}},
        {"a{1,2}?", "aaa", 0, 1, 0, {{0}}},
        {"a{2,4}?", "aaaaa", 0, 2, 0, {{0}}},
        {"a{2,}?", "aaaaa", 0, 2, 0, {{0}}},
        {"a{0}b", "ab", 1, 2, 0, {{0}}},
        {"a{3}", "aa", -1, 0, 0, {{0}}},
        {"x*", "", 0, 0, 0, {{0}}},
        {"(?:ab){2}", "ababab", 0, 4, 0, {{0}}},
        // classes
        {"[abc]+", "xxcabxx", 2, 5, 0, {{0}}},
        {"[^abc]+", "abxyc", 2, 4, 0, {{0}}},
        {"[a-c0-2]+", "z1b9", 1, 3, 0, {{0}}},
        {"[]a]+", "x]a]", 1, 4, 0, {{0}}},
        {"[^]a]+", "]ab]", 2, 3, 0, {{0}}},
        {"[a-]+", "x-a", 1, 3, 0, {{0}}},
        {"[-a]+", "x-a", 1, 3, 0, {{0}}},
        {"[\\]]", "]", 0, 1, 0, {{0}}},
        {"[\\d_]+", "ab12_", 2, 5, 0, {{0}}},
        {"[\\w.-]+", "!a.b-c!", 1, 6, 0, {{0}}},
        {"[\\D]+", "12ab3", 2, 4, 0, {{0}}},
        {"[\\x41-\\x43]+", "xABCD", 1, 4, 0, {{0}}},
        {"[\\n]", "a\nb", 1, 2, 0, {{0}}},
        {"[.]", "a.b", 1, 2, 0, {{0}}},
        {"[a[]+", "x[a", 1, 3, 0, {{0}}},
        {"\\d+", "ab123c", 2, 5, 0, {{0}}},
        {"\\D+", "12ab3", 2, 4, 0, {{0}}},
        {"\\w+", "  ab_1 ", 2, 6, 0, {{0}}},
        {"\\W+", "ab  c", 2, 4, 0, {{0}}},
        {"\\s+", "a \t\nb", 1, 4, 0, {{0}}},
        {"\\S+", "  ab ", 2, 4, 0, {{0}}},
        // boundaries and anchors
        {"\\bfoo\\b", "a foo b", 2, 5, 0, {{0}}},
        {"\\bfoo\\b", "afoo", -1, 0, 0, {{0}}},
        {"\\bfoo", "foo", 0, 3, 0, {{0}}},
        {"\\Boo", "foo", 1, 3, 0, {{0}}},
        {"\\b", "", -1, 0, 0, {{0}}},
        {"\\B", "", 0, 0, 0, {{0}}},
        {"^abc", "abc", 0, 3, 0, {{0}}},
        {"^abc", "xabc", -1, 0, 0, {{0}}},
        {"abc$", "abcx", -1, 0, 0, {{0}}},
        {"abc$", "xabc", 1, 4, 0, {{0}}},
        {"^$", "", 0, 0, 0, {{0}}},
        {"^$", "\n", -1, 0, 0, {{0}}},
        {"$", "abc", 3, 3, 0, {{0}}},
        {"^", "abc", 0, 0, 0, {{0}}},
        {"a^b", "a^b", -1, 0, 0, {{0}}},
        {"^a|b", "cb", 1, 2, 0, {{0}}},
        {"^a|b", "ca", -1, 0, 0, {{0}}},
        // nullable loop bodies: an empty iteration leaves the loop (Perl)
        {"(|a)*", "a", 0, 0, 1, {{0, 0}}},
        {"(a?" "?)*", "a", 0, 0, 1, {{0, 0}}},
        {"(a*)*", "b", 0, 0, 1, {{0, 0}}},
        // differs from Perl, which reports group 1 as 1..1; pinned as the engine's behaviour
        {"(a?)+", "a", 0, 1, 1, {{0, 1}}},
        // empty pattern and empty text
        {"", "abc", 0, 0, 0, {{0}}},
        {"", "", 0, 0, 0, {{0}}},
        {"x", "", -1, 0, 0, {{0}}},
        // alternation priority
        {"a|ab", "ab", 0, 1, 0, {{0}}},
        {"ab|a", "ab", 0, 2, 0, {{0}}},
        {"a|b|c", "xc", 1, 2, 0, {{0}}},
        {"a||b", "b", 0, 0, 0, {{0}}},
        {"(a|ab)(c|bcd)(d*)", "abcd", 0, 4, 3, {{0, 1}, {1, 4}, {4, 4}}},
        {"(a*)(a+)", "aaaa", 0, 4, 2, {{0, 3}, {3, 4}}},
        {"(a+?)(a+)", "aaaa", 0, 4, 2, {{0, 1}, {1, 4}}},
        // groups
        {"(a)(b)?", "ac", 0, 1, 2, {{0, 1}, {-1, -1}}},
        {"(a)|(b)", "b", 0, 1, 2, {{-1, -1}, {0, 1}}},
        {"((a)(b))", "ab", 0, 2, 3, {{0, 2}, {0, 1}, {1, 2}}},
        {"(a|b)+", "ab", 0, 2, 1, {{1, 2}}},
        {"(ab)+", "ababx", 0, 4, 1, {{2, 4}}},
        {"(?:ab)+", "ababx", 0, 4, 0, {{0}}},
        {"(a(b)?)+", "aba", 0, 3, 2, {{2, 3}, {1, 2}}},
        {"()", "x", 0, 0, 1, {{0, 0}}},
        {"(a){0}b", "ab", 1, 2, 1, {{-1, -1}}},
        // flags
        {"(?i)abc", "xAbC", 1, 4, 0, {{0}}},
        {"(?i)[a-c]+", "xABCx", 1, 4, 0, {{0}}},
        {"(?i)[^a]", "Ab", 1, 2, 0, {{0}}},
        {"(?i)\\x41", "a", 0, 1, 0, {{0}}},
        {"(?i)1a", "1A", 0, 2, 0, {{0}}},
        {"(?is)a.b", "A\nB", 0, 3, 0, {{0}}},
        {"(?si)a.b", "A\nB", 0, 3, 0, {{0}}},
        {"(?i)a.b", "A\nB", -1, 0, 0, {{0}}},
        {"abc", "ABC", -1, 0, 0, {{0}}},
        // realistic
        {"(?i)<script", "x<SCRIPT>", 1, 8, 0, {{0}}},
        {"(?i)drop\\s+table", "x DROP \n TaBlE", 2, 14, 0, {{0}}},
        {"\\b\\d{3}-\\d{2}-\\d{4}\\b", "ssn 123-45-6789.", 4, 15, 0, {{0}}},
        {"\\b\\d{3}-\\d{2}-\\d{4}\\b", "1123-45-6789", -1, 0, 0, {{0}}},
        {"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}", "mail bob.smith@ex-ample.co.uk now", 5, 29, 0, {{0}}},
        {"user=(\\w+)", "id=1 user=alice_9; x", 5, 17, 1, {{10, 17}}},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        run_case(&cases[i]);
    }
}

static void test_empty_loops(void) {
    static const struct { const char *pat; const char *text; bool match; } cases[] = {
        {"(a*)*b", "aab", true},
        {"(a*)*b", "aac", false},
        {"(a*)+", "aa", true},
        {"(|a)+", "aa", true},
        {"(|a)+b", "aab", true},
        {"(a|)*c", "aaac", true},
        {"(a*)*$", "aaa", true},
        {"(?:a?)*b", "aaab", true},
        {"(?:)*x", "x", true},
        {"(?:a*)*?x", "aax", true},
        {"((?:a*)+)*y", "aaay", true},
        {"(?:(?:)){255}x", "x", true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        pb_arena arena = {0};
        pb_regex *re = compile_ok(&arena, sl(cases[i].pat));
        pb_regex_span spans[10];
        CHECK(t_test(re, sl(cases[i].text)) == cases[i].match);
        CHECK(t_search(re, sl(cases[i].text), 0, spans, 10) == cases[i].match);
        pb_arena_free(&arena);
    }
}

static void test_binary(void) {
    pb_arena arena = {0};
    pb_regex_span sp[2];

    const char nul_text[] = {'a', '\0', 'b'};
    pb_slice text = {nul_text, 3};
    pb_regex *re = compile_ok(&arena, sl("a.b"));
    CHECK(t_search(re, text, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 3);
    re = compile_ok(&arena, sl("a\\x00b"));
    CHECK(t_search(re, text, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 3);
    re = compile_ok(&arena, sl("a\\x00c"));
    CHECK(!t_test(re, text));
    re = compile_ok(&arena, sl("[^a]"));
    CHECK(t_search(re, text, 0, sp, 2) && sp[0].start == 1 && sp[0].end == 2);
    re = compile_ok(&arena, sl("[\\x00-\\x1f]+"));
    CHECK(t_search(re, text, 0, sp, 2) && sp[0].start == 1 && sp[0].end == 2);
    re = compile_ok(&arena, sl("b$"));
    CHECK(t_test(re, text));

    const char nul_pat[] = {'a', '\0', 'b'};
    re = compile_ok(&arena, (pb_slice){nul_pat, 3});
    CHECK(t_search(re, text, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 3);
    CHECK(!t_test(re, sl("ab")));
    const char esc_nul_pat[] = {'a', '\\', '\0', 'b'};
    re = compile_ok(&arena, (pb_slice){esc_nul_pat, 4});
    CHECK(t_test(re, text));
    CHECK(pb_regex_source(re).len == 4);

    const char high[] = {(char)0x80, (char)0xFF, (char)0xFE, 'x'};
    pb_slice htext = {high, 4};
    re = compile_ok(&arena, sl("[\\x80-\\xff]+"));
    CHECK(t_search(re, htext, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 3);
    re = compile_ok(&arena, sl("[^\\x80-\\xfe]"));
    CHECK(t_search(re, htext, 0, sp, 2) && sp[0].start == 1 && sp[0].end == 2);
    re = compile_ok(&arena, sl("[^\\x00-\\x7f]+x"));
    CHECK(t_search(re, htext, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 4);
    re = compile_ok(&arena, sl("\\W+"));
    CHECK(t_search(re, htext, 0, sp, 2) && sp[0].start == 0 && sp[0].end == 3);

    re = compile_ok(&arena, sl("caf\xc3\xa9"));
    CHECK(t_search(re, sl("un caf\xc3\xa9"), 0, sp, 2) && sp[0].start == 3 && sp[0].end == 8);
    re = compile_ok(&arena, sl("caf."));
    CHECK(t_search(re, sl("caf\xc3\xa9"), 0, sp, 2) && sp[0].end == 4);
    re = compile_ok(&arena, sl("[\xc3]\xa9"));
    CHECK(t_test(re, sl("caf\xc3\xa9")));

    re = compile_ok(&arena, sl("a*"));
    CHECK(t_test(re, (pb_slice){NULL, 0}));
    re = compile_ok(&arena, (pb_slice){NULL, 0});
    CHECK(t_test(re, (pb_slice){NULL, 0}));
    pb_arena_free(&arena);
}

static void test_from_offsets(void) {
    pb_arena arena = {0};
    pb_regex_span sp[2];
    pb_regex *re = compile_ok(&arena, sl("a"));
    pb_slice t = sl("aaa");
    CHECK(t_search(re, t, 0, sp, 2) && sp[0].start == 0);
    CHECK(t_search(re, t, 1, sp, 2) && sp[0].start == 1 && sp[0].end == 2);
    CHECK(t_search(re, t, 2, sp, 2) && sp[0].start == 2);
    CHECK(!t_search(re, t, 3, sp, 2));
    CHECK(!t_search(re, t, 4, sp, 2));
    CHECK(!t_search(re, t, (size_t)-1, sp, 2));

    re = compile_ok(&arena, sl("a*"));
    CHECK(t_search(re, sl("baa"), 3, sp, 2) && sp[0].start == 3 && sp[0].end == 3);
    CHECK(t_search(re, sl(""), 0, sp, 2) && sp[0].start == 0 && sp[0].end == 0);

    re = compile_ok(&arena, sl("$"));
    CHECK(t_search(re, sl("ab"), 2, sp, 2) && sp[0].start == 2);

    re = compile_ok(&arena, sl("^a"));
    CHECK(t_search(re, sl("aa"), 0, sp, 2));
    CHECK(!t_search(re, sl("aa"), 1, sp, 2));

    // \b still sees the byte before `from`.
    re = compile_ok(&arena, sl("\\bx"));
    CHECK(!t_search(re, sl("ax"), 1, sp, 2));
    CHECK(t_search(re, sl("a x"), 1, sp, 2) && sp[0].start == 2);

    re = compile_ok(&arena, sl("\\d+"));
    pb_slice text = sl("a1b22c333");
    static const size_t want[3][2] = {{1, 2}, {3, 5}, {6, 9}};
    size_t from = 0;
    for (size_t i = 0; i < 3; i += 1) {
        CHECK(t_search(re, text, from, sp, 1));
        CHECK(sp[0].start == want[i][0] && sp[0].end == want[i][1]);
        from = sp[0].end;
    }
    CHECK(!t_search(re, text, from, sp, 1));

    re = compile_ok(&arena, sl("x*"));
    size_t count = 0;
    from = 0;
    while (t_search(re, sl("axb"), from, sp, 1)) {
        count += 1;
        from = sp[0].end > sp[0].start ? sp[0].end : sp[0].end + 1;
        if (from > 3) break;
    }
    CHECK(count == 4); // "" at 0, "x" at 1, "" at 2, "" at 3

    re = compile_ok(&arena, sl("(a)(b)(c)"));
    CHECK(pb_regex_group_count(re) == 3);
    pb_regex_span many[6];
    memset(many, 0x55, sizeof many);
    CHECK(t_search(re, sl("abc"), 0, many, 6));
    CHECK(many[3].matched && many[3].start == 2 && many[3].end == 3);
    for (size_t i = 4; i < 6; i += 1) {
        CHECK(many[i].start == (size_t)0x55555555u * (SIZE_MAX / UINT32_MAX) && many[i].end == (size_t)0x55555555u * (SIZE_MAX / UINT32_MAX));
    }
    pb_regex_span two[2];
    CHECK(t_search(re, sl("abc"), 0, two, 2));
    CHECK(two[0].end == 3 && two[1].start == 0 && two[1].end == 1);
    CHECK(t_search(re, sl("abc"), 0, NULL, 0));
    CHECK(t_search(re, sl("abc"), 0, two, 0));
    pb_arena_free(&arena);
}

static void expect_error(const char *pat, pb_regex_error code, size_t off) {
    pb_arena arena = {0};
    pb_regex *re = (pb_regex *)1;
    size_t got_off = 9999;
    char msg[96] = "";
    pb_regex_error err = pb_regex_compile(&arena, sl(pat), &re, &got_off, msg, sizeof msg);
    if (err != code || (off == SIZE_MAX ? got_off > strlen(pat) : got_off != off) || re != NULL || msg[0] == '\0') {
        fprintf(stderr, "error case failed: /%s/ got %s at %zu (%s), want %s at %zu\n", pat, pb_regex_error_name(err), got_off, msg, pb_regex_error_name(code), off);
    }
    CHECK(err == code);
    CHECK(off == SIZE_MAX ? got_off <= strlen(pat) : got_off == off);
    CHECK(re == NULL);
    CHECK(msg[0] != '\0');
    pb_arena_free(&arena);
}

static void test_errors(void) {
    // syntax
    expect_error("abc(", PB_REGEX_SYNTAX, 3);
    expect_error("(a", PB_REGEX_SYNTAX, 0);
    expect_error("(?:a", PB_REGEX_SYNTAX, 0);
    expect_error("a)", PB_REGEX_SYNTAX, 1);
    expect_error("[abc", PB_REGEX_SYNTAX, 0);
    expect_error("x[a", PB_REGEX_SYNTAX, 1);
    expect_error("[", PB_REGEX_SYNTAX, 0);
    expect_error("[]", PB_REGEX_SYNTAX, 0);
    expect_error("[^]", PB_REGEX_SYNTAX, 0);
    expect_error("[^\\x00-\\xff]", PB_REGEX_SYNTAX, 0);
    expect_error("[z-a]", PB_REGEX_SYNTAX, 3);
    expect_error("[a-\\d]", PB_REGEX_SYNTAX, 3);
    expect_error("[a\\", PB_REGEX_SYNTAX, 2);
    expect_error("a\\", PB_REGEX_SYNTAX, 1);
    expect_error("\\", PB_REGEX_SYNTAX, 0);
    expect_error("\\x4", PB_REGEX_SYNTAX, 0);
    expect_error("\\xZZ", PB_REGEX_SYNTAX, 0);
    expect_error("a{2,1}", PB_REGEX_SYNTAX, 1);
    expect_error("a{", PB_REGEX_SYNTAX, 1);
    expect_error("a{x}", PB_REGEX_SYNTAX, 1);
    expect_error("a{,2}", PB_REGEX_SYNTAX, 1);
    expect_error("a{2", PB_REGEX_SYNTAX, 1);
    expect_error("a{2,", PB_REGEX_SYNTAX, 1);
    expect_error("a{2,x}", PB_REGEX_SYNTAX, 1);
    expect_error("{2}", PB_REGEX_SYNTAX, 0);
    expect_error("*a", PB_REGEX_SYNTAX, 0);
    expect_error("+", PB_REGEX_SYNTAX, 0);
    expect_error("a|*", PB_REGEX_SYNTAX, 2);
    expect_error("(*)", PB_REGEX_SYNTAX, 1);
    expect_error("a**", PB_REGEX_SYNTAX, 2);
    expect_error("a*?*", PB_REGEX_SYNTAX, 3);
    expect_error("a+*", PB_REGEX_SYNTAX, 2);
    expect_error("a{2}{3}", PB_REGEX_SYNTAX, 4);
    expect_error("^*", PB_REGEX_SYNTAX, 1);
    expect_error("\\b+", PB_REGEX_SYNTAX, 2);
    expect_error("$?", PB_REGEX_SYNTAX, 1);
    // quantified assertions are rejected through plain and capturing groups alike
    expect_error("(?:\\b)?a", PB_REGEX_SYNTAX, 6);
    expect_error("(\\b)?a", PB_REGEX_SYNTAX, 4);
    expect_error("(?:^)*a", PB_REGEX_SYNTAX, 5);
    expect_error("(^)*a", PB_REGEX_SYNTAX, 3);
    expect_error("{\"user\":\"(\\w+)\"", PB_REGEX_SYNTAX, 0);
    expect_error("(?", PB_REGEX_SYNTAX, 0);
    // unsupported
    expect_error("a*+", PB_REGEX_UNSUPPORTED, 2);
    expect_error("a++", PB_REGEX_UNSUPPORTED, 2);
    expect_error("(a)\\1", PB_REGEX_UNSUPPORTED, 3);
    expect_error("(?=a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?!a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?<=a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?<!a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?<n>a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?P<n>a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?>a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("a(?i)", PB_REGEX_UNSUPPORTED, 1);
    expect_error("(?i)(?s)a", PB_REGEX_UNSUPPORTED, 4);
    expect_error("(?i:a)", PB_REGEX_UNSUPPORTED, 0);
    expect_error("(?x)a", PB_REGEX_UNSUPPORTED, 0);
    expect_error("[[:alpha:]]", PB_REGEX_UNSUPPORTED, 1);
    expect_error("[a[.x.]]", PB_REGEX_UNSUPPORTED, 2);
    expect_error("\\p{L}", PB_REGEX_UNSUPPORTED, 0);
    expect_error("\\q", PB_REGEX_UNSUPPORTED, 0);
    expect_error("\\A", PB_REGEX_UNSUPPORTED, 0);
    expect_error("[\\b]", PB_REGEX_UNSUPPORTED, 1);
    expect_error("[\\1]", PB_REGEX_UNSUPPORTED, 1);
    // caps
    expect_error("a{256}", PB_REGEX_TOO_LARGE, 4);
    expect_error("a{1,256}", PB_REGEX_TOO_LARGE, 6);
    expect_error("a{99999999999999999999}", PB_REGEX_TOO_LARGE, 4);
    expect_error("(?:a{200}){200}", PB_REGEX_TOO_LARGE, SIZE_MAX);
    expect_error("(?:a{255}){5}", PB_REGEX_TOO_LARGE, SIZE_MAX);
    expect_error("(?:(?:(?:a{255}){255}){255}){255}", PB_REGEX_TOO_LARGE, SIZE_MAX);
    expect_error("()()()()()()()()()()", PB_REGEX_TOO_MANY_GROUPS, 18);
}

static void test_caps(void) {
    pb_arena arena = {0};

    // Pattern length: exactly the cap compiles, one more does not.
    char big[PB_REGEX_MAX_PATTERN + 8];
    memset(big, 'a', sizeof big);
    pb_regex *re = NULL;
    size_t off = 0;
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN - 1}, &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(t_test(re, (pb_slice){big, PB_REGEX_MAX_PATTERN - 1}));
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN + 1}, &re, &off, NULL, 0) == PB_REGEX_TOO_LONG);
    CHECK(re == NULL && off == PB_REGEX_MAX_PATTERN);
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN + 8}, &re, &off, NULL, 0) == PB_REGEX_TOO_LONG);

    // Program size: 1000 instructions plus MATCH fits; 1024 literals plus MATCH does not.
    CHECK(pb_regex_compile(&arena, sl("(?:a{100}){10}"), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN}, &re, &off, NULL, 0) == PB_REGEX_TOO_LARGE);
    CHECK(pb_regex_compile(&arena, sl("(?:(?:(?:){255}){255}){255}"), &re, &off, NULL, 0) == PB_REGEX_OK);

    // Groups: nine fit, ten do not.
    CHECK(pb_regex_compile(&arena, sl("(a)(b)(c)(d)(e)(f)(g)(h)(i)"), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(pb_regex_group_count(re) == 9);
    pb_regex_span sp[10];
    CHECK(t_search(re, sl("abcdefghi"), 0, sp, 10) && sp[9].start == 8 && sp[9].end == 9);
    CHECK(pb_regex_compile(&arena, sl("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)"), &re, &off, NULL, 0) == PB_REGEX_TOO_MANY_GROUPS);
    CHECK(pb_regex_compile(&arena, sl("(?:a)(?:b)(?:c)(?:d)(?:e)(?:f)(?:g)(?:h)(?:i)(?:j)(a)"), &re, &off, NULL, 0) == PB_REGEX_OK);

    // Nesting: 32 deep compiles, 33 does not.
    char deep[256];
    for (int depth = 32; depth <= 33; depth += 1) {
        size_t n = 0;
        for (int i = 0; i < depth; i += 1) {
            memcpy(deep + n, "(?:", 3);
            n += 3;
        }
        deep[n++] = 'a';
        for (int i = 0; i < depth; i += 1) deep[n++] = ')';
        pb_regex_error err = pb_regex_compile(&arena, (pb_slice){deep, n}, &re, &off, NULL, 0);
        CHECK(err == (depth == 32 ? PB_REGEX_OK : PB_REGEX_TOO_DEEP));
        if (depth == 33) CHECK(off == 3u * 32 + 3);
    }
    memset(big, '(', PB_REGEX_MAX_PATTERN);
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN}, &re, &off, NULL, 0) != PB_REGEX_OK);
    memset(big, '[', PB_REGEX_MAX_PATTERN);
    CHECK(pb_regex_compile(&arena, (pb_slice){big, PB_REGEX_MAX_PATTERN}, &re, &off, NULL, 0) != PB_REGEX_OK);

    char msg[8];
    CHECK(pb_regex_compile(&arena, sl("[abc"), &re, &off, msg, sizeof msg) == PB_REGEX_SYNTAX);
    CHECK(strlen(msg) == 7);
    CHECK(pb_regex_compile(&arena, sl("[abc"), &re, &off, msg, 1) == PB_REGEX_SYNTAX);
    CHECK(msg[0] == '\0');
    CHECK(pb_regex_compile(&arena, sl("[abc"), NULL, NULL, NULL, 0) == PB_REGEX_SYNTAX);
    CHECK(pb_regex_compile(&arena, sl("abc"), NULL, NULL, NULL, 0) == PB_REGEX_OK);

    CHECK(pb_regex_compile(&arena, sl("(?i)ab(c)"), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(pb_slice_eq_lit(pb_regex_source(re), "(?i)ab(c)"));
    CHECK(pb_regex_group_count(re) == 1);
    CHECK(strcmp(pb_regex_error_name(PB_REGEX_OK), "ok") == 0);
    CHECK(pb_regex_error_name(PB_REGEX_SYNTAX)[0] != '\0');
    pb_arena_free(&arena);
}

// The pattern bytes are copied: the caller may reuse its buffer right after compile.
static void test_pattern_copied(void) {
    pb_arena arena = {0};
    char buf[16];
    memcpy(buf, "ab+c", 5);
    pb_regex *re = compile_ok(&arena, (pb_slice){buf, 4});
    memset(buf, 'z', sizeof buf);
    CHECK(t_test(re, sl("xabbbc")));
    CHECK(pb_slice_eq_lit(pb_regex_source(re), "ab+c"));
    pb_arena_free(&arena);
}

// Wall-clock bound for the budget tests; sanitizer builds run several times slower.
#if MB_TEST_ASAN
#define PERF_LIMIT_SECONDS 20.0
#else
#define PERF_LIMIT_SECONDS 2.0
#endif

static double seconds_since(clock_t start) {
    return (double)(clock() - start) / CLOCKS_PER_SEC;
}

// Exponential under backtracking; the Pike VM must stay O(text * program).
static void test_linearity(void) {
    enum { N = 64 * 1024 };
    static char as[N + 1], xs[N + 1];
    memset(as, 'a', N);
    memset(xs, 'x', N);
    as[N] = 'b';
    xs[N] = 'z';
    static const struct { const char *pat; const char *kind; bool expect; } cases[] = {
        {"(a*)*b", "a", true},
        {"(a*)*c", "a", false},
        {"(a|aa)+$", "a", false},
        {"(a|aa)+c", "a", false},
        {"(x+x+)+y", "x", false},
        {"(?:x+x+)+y", "x", false},
        {"(a?){25}a{25}", "a", true},
        {"(a?){25}a{25}c", "a", false},
        {"(?:a|a)*c", "a", false},
        {"(.*)(.*)(.*)(.*)(.*)c", "a", false},
        {"(a+)+$", "a", false},
    };
    clock_t start = clock();
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        pb_arena arena = {0};
        pb_regex *re = compile_ok(&arena, sl(cases[i].pat));
        pb_slice text = {cases[i].kind[0] == 'a' ? as : xs, N + 1};
        CHECK(t_test(re, text) == cases[i].expect);
        pb_regex_span sp[10];
        bool hit = t_search(re, text, 0, sp, 10);
        CHECK(t_test(re, text) == hit);
        if (hit) CHECK(sp[0].start <= sp[0].end && sp[0].end <= text.len);
        pb_slice shortened = {text.ptr, N};
        CHECK(t_test(re, shortened) == t_search(re, shortened, 0, sp, 10));
        pb_arena_free(&arena);
    }
    CHECK(seconds_since(start) < 20.0);

    pb_arena arena = {0};
    CHECK(t_test(compile_ok(&arena, sl("(a*)*b")), sl("aaaab")));
    CHECK(!t_test(compile_ok(&arena, sl("(a*)*b")), sl("aaaa")));
    CHECK(t_test(compile_ok(&arena, sl("(a?){25}a{25}")), sl("aaaaaaaaaaaaaaaaaaaaaaaaa")));
    CHECK(!t_test(compile_ok(&arena, sl("(a?){25}a{25}")), sl("aaaaaaaaaaaaaaaaaaaaaaaa")));
    pb_arena_free(&arena);
}

// Prefix skipping and anchoring must not change results.
static void test_fast_paths(void) {
    pb_arena arena = {0};
    pb_regex_span sp[2];
    pb_regex *re = compile_ok(&arena, sl("foo\\d"));
    CHECK(t_search(re, sl("fofofoo foo1"), 0, sp, 2) && sp[0].start == 8 && sp[0].end == 12);
    CHECK(!t_search(re, sl("fofofoo foo"), 0, sp, 2));
    CHECK(!t_search(re, sl("zzzzzzzz"), 0, sp, 2));
    re = compile_ok(&arena, sl("(foo|fob)x"));
    CHECK(t_search(re, sl("fobfobx"), 0, sp, 2) && sp[0].start == 3);
    re = compile_ok(&arena, sl("\\bfoo"));
    CHECK(!t_test(re, sl("xfoo")) && t_test(re, sl("x foo")));
    re = compile_ok(&arena, sl("(?:^a|^b)c"));
    CHECK(t_test(re, sl("bc")) && !t_test(re, sl("xbc")));
    // Anchored pattern on a long text that never matches must not rescan from every offset.
    enum { N = 256 * 1024 };
    static char buf[N];
    memset(buf, 'a', N);
    re = compile_ok(&arena, sl("^(a*)*b"));
    clock_t start = clock();
    for (int i = 0; i < 50; i += 1) {
        CHECK(!t_test(re, (pb_slice){buf, N}));
    }
    CHECK(seconds_since(start) < 20.0);
    pb_arena_free(&arena);
}

// ---------- randomised tests ----------

// xorshift64* generator; fixed seeds keep failures reproducible.
typedef struct rng {
    uint64_t s;
} rng;

static uint64_t rng_next(rng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static unsigned rng_below(rng *r, unsigned n) {
    return (unsigned)((rng_next(r) >> 33) % n);
}

typedef struct gen_buf {
    char s[256];
    size_t n;
} gen_buf;

static void gen_put(gen_buf *g, const char *lit) {
    size_t len = strlen(lit);
    if (g->n + len < sizeof g->s - 1) {
        memcpy(g->s + g->n, lit, len);
        g->n += len;
    }
}

static void gen_alt(rng *r, gen_buf *g, int depth);

// Anchors are allowed only in a single-branch top-level pattern: libc ERE mishandles them next to '|' and inside groups.

// Emits one concatenation drawn from the subset where POSIX ERE and this engine agree.
static void gen_branch(rng *r, gen_buf *g, int depth, bool anchors) {
    static const char *atoms[] = {"a", "b", "c", "a", "b", ".", "[ab]", "[^a]", "[a-c]", "[bc]"};
    static const char *quants[] = {"", "", "", "*", "+", "?"};
    unsigned items = 1 + rng_below(r, 4);
    if (anchors && rng_below(r, 8) == 0) gen_put(g, "^");
    for (unsigned i = 0; i < items; i += 1) {
        if (depth < 3 && rng_below(r, 5) == 0) {
            gen_put(g, "(");
            gen_alt(r, g, depth + 1);
            gen_put(g, ")");
        } else {
            gen_put(g, atoms[rng_below(r, sizeof atoms / sizeof atoms[0])]);
        }
        gen_put(g, quants[rng_below(r, sizeof quants / sizeof quants[0])]);
    }
    if (anchors && rng_below(r, 8) == 0) gen_put(g, "$");
}

static void gen_alt(rng *r, gen_buf *g, int depth) {
    unsigned branches = 1 + (rng_below(r, 4) == 0 ? 1 + rng_below(r, 2) : 0);
    for (unsigned i = 0; i < branches; i += 1) {
        if (i > 0) gen_put(g, "|");
        gen_branch(r, g, depth, depth == 0 && branches == 1);
    }
}

// Oracle comparison is boolean only, using the C library's POSIX ERE matcher.
static void test_differential(void) {
    rng r = {0x9E3779B97F4A7C15ULL};
    size_t compared = 0, skipped = 0, matches = 0, dfa_patterns = 0;
    for (int iter = 0; iter < 4000; iter += 1) {
        gen_buf g = {{0}, 0};
        gen_alt(&r, &g, 0);
        g.s[g.n] = '\0';

        regex_t oracle;
        pb_arena arena = {0};
        pb_regex *re = NULL, *pike = NULL;
        size_t off = 0;
        if (regcomp(&oracle, g.s, REG_EXTENDED | REG_NOSUB) != 0) {
            skipped += 1;
            continue;
        }
        if (pb_regex_compile(&arena, (pb_slice){g.s, g.n}, &re, &off, NULL, 0) != PB_REGEX_OK || pb_regex_compile(&arena, (pb_slice){g.s, g.n}, &pike, &off, NULL, 0) != PB_REGEX_OK) {
            regfree(&oracle);
            pb_arena_free(&arena);
            skipped += 1;
            continue;
        }
        pb_regex_disable_dfa(pike);
        dfa_patterns += pb_regex_has_dfa(re) ? 1 : 0;
        for (int t = 0; t < 8; t += 1) {
            char text[16];
            size_t len = rng_below(&r, 13);
            for (size_t i = 0; i < len; i += 1) text[i] = "aabbcd"[rng_below(&r, 6)];
            text[len] = '\0';
            bool want = regexec(&oracle, text, 0, NULL, 0) == 0;
            bool got = t_test(re, (pb_slice){text, len});
            pb_regex_span sp[10], sp_pike[10];
            bool searched = t_search(re, (pb_slice){text, len}, 0, sp, 10);
            bool pike_test = t_test(pike, (pb_slice){text, len});
            bool pike_search = t_search(pike, (pb_slice){text, len}, 0, sp_pike, 10);
            if (want != got || searched != got || pike_test != want || pike_search != want) {
                fprintf(stderr, "differential mismatch: /%s/ on \"%s\" oracle=%d dfa_test=%d search=%d pike_test=%d pike_search=%d\n", g.s, text, want, got, searched, pike_test, pike_search);
            }
            CHECK(want == got);
            CHECK(searched == got);
            CHECK(pike_test == want && pike_search == want);
            if (want) CHECK(sp[0].start == sp_pike[0].start && sp[0].end == sp_pike[0].end);
            for (size_t f = 0; f <= len; f += 1) {
                pb_regex_span fa[10], fb[10];
                bool ha = t_search(re, (pb_slice){text, len}, f, fa, 10);
                CHECK(ha == t_search(pike, (pb_slice){text, len}, f, fb, 10));
                for (size_t gi = 0; ha && gi <= pb_regex_group_count(re); gi += 1) {
                    CHECK(fa[gi].matched == fb[gi].matched && fa[gi].start == fb[gi].start && fa[gi].end == fb[gi].end);
                }
            }
            compared += 1;
            matches += want ? 1 : 0;
        }
        regfree(&oracle);
        pb_arena_free(&arena);
    }
    printf("differential: %zu comparisons (%zu matching), %zu patterns skipped, %zu with a DFA\n", compared, matches, skipped, dfa_patterns);
    CHECK(dfa_patterns > 3000);
    CHECK(compared > 10000);
    CHECK(matches > 1000 && matches < compared - 1000);
}

static void check_spans(const pb_regex_span *sp, size_t n, size_t from, size_t len) {
    CHECK(sp[0].matched && sp[0].start >= from && sp[0].start <= sp[0].end && sp[0].end <= len);
    for (size_t i = 1; i < n; i += 1) {
        if (sp[i].matched) {
            CHECK(sp[i].start <= sp[i].end && sp[i].end <= len);
            CHECK(sp[i].start >= sp[0].start && sp[i].end <= sp[0].end);
        } else {
            CHECK(sp[i].start == 0 && sp[i].end == 0);
        }
    }
}

// Poor man's fuzzer, meant for ASAN. Apple clang's ASAN runtime hangs on recent
// macOS; use Homebrew LLVM (-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang -DMB_ASAN=ON) or Linux.
static void test_random_stress(void) {
    static const char soup[] = "()[]{}*+?|\\.^$-:=<>!dDwWsSbBxnrtip0123456789,abcAB \n_";
    rng r = {0x1234567887654321ULL};
    size_t compiled = 0, rejected = 0, with_dfa = 0;
    for (int iter = 0; iter < 30000; iter += 1) {
        char pat[48];
        size_t plen = rng_below(&r, sizeof pat + 1);
        bool raw = rng_below(&r, 6) == 0;
        for (size_t i = 0; i < plen; i += 1) {
            pat[i] = raw ? (char)rng_next(&r) : soup[rng_below(&r, sizeof soup - 1)];
        }
        if (plen >= 4 && rng_below(&r, 10) == 0) memcpy(pat, "(?i)", 4);
        pb_arena arena = {0};
        pb_regex *re = NULL;
        size_t off = 0;
        char msg[64];
        pb_regex_error err = pb_regex_compile(&arena, (pb_slice){pat, plen}, &re, &off, msg, sizeof msg);
        if (err != PB_REGEX_OK) {
            CHECK(re == NULL && off <= plen && msg[0] != '\0');
            rejected += 1;
            pb_arena_free(&arena);
            continue;
        }
        compiled += 1;
        pb_regex *pike = NULL;
        CHECK(pb_regex_compile(&arena, (pb_slice){pat, plen}, &pike, &off, NULL, 0) == PB_REGEX_OK);
        pb_regex_disable_dfa(pike);
        with_dfa += pb_regex_has_dfa(re) ? 1 : 0;
        for (int t = 0; t < 6; t += 1) {
            char text[80];
            size_t len = rng_below(&r, sizeof text + 1);
            bool texty = rng_below(&r, 2) == 0;
            for (size_t i = 0; i < len; i += 1) {
                text[i] = texty ? "abcAB _\n019-"[rng_below(&r, 12)] : (char)rng_next(&r);
            }
            pb_slice ts = {text, len};
            bool via_test = t_test(re, ts);
            pb_regex_span full[10], one[1], pfull[10];
            bool via_search = t_search(re, ts, 0, full, 10);
            CHECK(via_test == via_search);
            CHECK(t_test(pike, ts) == via_test);
            CHECK(t_search(pike, ts, 0, pfull, 10) == via_search);
            if (via_search) {
                for (size_t gi = 0; gi <= pb_regex_group_count(re); gi += 1) {
                    CHECK(full[gi].matched == pfull[gi].matched && full[gi].start == pfull[gi].start && full[gi].end == pfull[gi].end);
                }
            }
            CHECK(t_search(re, ts, 0, one, 1) == via_search);
            size_t groups = pb_regex_group_count(re);
            if (via_search) {
                check_spans(full, groups + 1, 0, len);
                CHECK(one[0].start == full[0].start && one[0].end == full[0].end);
                pb_regex_span again[10];
                CHECK(t_search(re, ts, full[0].start, again, 10));
                CHECK(again[0].start == full[0].start && again[0].end == full[0].end);
            }
            size_t from = len == 0 ? 0 : rng_below(&r, (unsigned)len + 1);
            pb_regex_span at[10], pat_[10];
            bool at_hit = t_search(re, ts, from, at, 10);
            CHECK(t_search(pike, ts, from, pat_, 10) == at_hit);
            if (at_hit) {
                CHECK(at[0].start == pat_[0].start && at[0].end == pat_[0].end);
                check_spans(at, groups + 1, from, len);
                CHECK(via_test);
            }
            if (len <= 24) {
                for (size_t f = 0; f <= len; f += 1) {
                    pb_regex_span fa[10], fb[10];
                    bool ha = t_search(re, ts, f, fa, 10);
                    CHECK(ha == t_search(pike, ts, f, fb, 10));
                    for (size_t g = 0; ha && g <= groups; g += 1) {
                        CHECK(fa[g].matched == fb[g].matched && fa[g].start == fb[g].start && fa[g].end == fb[g].end);
                    }
                }
            }
            CHECK(!t_search(re, ts, len + 1, at, 10));
        }
        pb_arena_free(&arena);
    }
    printf("stress: %zu patterns compiled (%zu with a DFA), %zu rejected\n", compiled, with_dfa, rejected);
    CHECK(with_dfa > 1000);
    CHECK(compiled > 1000 && rejected > 1000);
}

// DFA construction caps: pathological patterns fall back to the Pike VM silently and stay correct.
static void test_dfa_caps(void) {
    pb_arena arena = {0};
    pb_regex *re = NULL;
    size_t off = 0;

    // Classic exponential-state pattern: needs 2^13 DFA states, so it must fall back.
    const char *blowup = "(a|b)*a(a|b){12}";
    CHECK(pb_regex_compile(&arena, sl(blowup), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(!pb_regex_has_dfa(re) && pb_regex_dfa_states(re) == 0);
    regex_t oracle;
    CHECK(regcomp(&oracle, blowup, REG_EXTENDED | REG_NOSUB) == 0);
    rng r = {0xDEADBEEFCAFEF00DULL};
    size_t hits = 0;
    for (int i = 0; i < 300; i += 1) {
        char text[48];
        size_t len = rng_below(&r, 45);
        for (size_t k = 0; k < len; k += 1) text[k] = "ab"[rng_below(&r, 2)];
        text[len] = '\0';
        bool want = regexec(&oracle, text, 0, NULL, 0) == 0;
        pb_regex_span sp[2];
        CHECK(t_test(re, (pb_slice){text, len}) == want);
        CHECK(t_search(re, (pb_slice){text, len}, 0, sp, 2) == want);
        hits += want ? 1 : 0;
    }
    regfree(&oracle);
    CHECK(hits > 50 && hits < 250);

    CHECK(pb_regex_compile(&arena, sl("(a|b)*a(a|b){4}"), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(pb_regex_has_dfa(re));
    CHECK(t_test(re, sl("bbbabbbb")) && !t_test(re, sl("bbbbbbbb")));

    char many[PB_REGEX_MAX_PATTERN];
    size_t n = 0;
    for (unsigned i = 0; i < 120 && n + 12 < sizeof many; i += 1) {
        n += (size_t)snprintf(many + n, sizeof many - n, "%sk%c%c%c%c%c", i ? "|" : "", 'a' + i % 26, 'a' + (i / 26) % 26, 'A' + i % 7, '0' + i % 10, 'a' + (i * 7) % 26);
    }
    CHECK(pb_regex_compile(&arena, (pb_slice){many, n}, &re, &off, NULL, 0) == PB_REGEX_OK);
    pb_regex *twin = NULL;
    CHECK(pb_regex_compile(&arena, (pb_slice){many, n}, &twin, &off, NULL, 0) == PB_REGEX_OK);
    pb_regex_disable_dfa(twin);
    for (int i = 0; i < 200; i += 1) {
        char text[64];
        size_t len = rng_below(&r, 40);
        for (size_t k = 0; k < len; k += 1) text[k] = "kabcdefAB01 "[rng_below(&r, 12)];
        CHECK(t_test(re, (pb_slice){text, len}) == t_test(twin, (pb_slice){text, len}));
    }
    CHECK(t_test(re, sl("xxkaaA0a")) == t_test(twin, sl("xxkaaA0a")));

    static const char *eligible[] = {"^abc", "abc$", "\\bfoo\\b", "\\Bx", "^$", "(?i)<script|drop\\s+table|\\$\\{jndi:", "\\b\\d{3}-\\d{2}-\\d{4}\\b", "a|b", "^(a|b)*$"};
    for (size_t i = 0; i < sizeof eligible / sizeof eligible[0]; i += 1) {
        CHECK(pb_regex_compile(&arena, sl(eligible[i]), &re, &off, NULL, 0) == PB_REGEX_OK);
        if (!pb_regex_has_dfa(re)) fprintf(stderr, "no DFA for %s\n", eligible[i]);
        CHECK(pb_regex_has_dfa(re));
        CHECK(pb_regex_dfa_states(re) >= 3);
    }

    CHECK(pb_regex_compile(&arena, sl("\\bfoo\\b"), &re, &off, NULL, 0) == PB_REGEX_OK);
    CHECK(t_test(re, sl("a foo.")));
    pb_regex_disable_dfa(re);
    CHECK(!pb_regex_has_dfa(re) && t_test(re, sl("a foo.")) && !t_test(re, sl("afoo.")));
    pb_arena_free(&arena);
}

// Assertions at DFA boundaries: from offsets feed the previous byte into the start state.
static void test_dfa_assertions(void) {
    static const struct { const char *pat; const char *text; size_t from; bool match; } cases[] = {
        {"\\bx", "ax", 1, false},
        {"\\bx", "a x", 1, true},
        {"\\Bx", "ax", 1, true},
        {"^a", "aa", 1, false},
        {"a$", "aa", 1, true},
        {"\\b", "a", 1, true},
        {"\\b", "a", 0, true},
        {"\\B", "a", 1, false},
        {"\\B", "", 0, true},
        {"$", "", 0, true},
        {"^$", "", 0, true},
        {"^$", "a", 0, false},
        {"x*$", "ax", 1, true},
        {"foo\\b", "foobar foo", 0, true},
        {"foo\\b", "foobar", 0, false},
        {"(?:\\bfoo|bar\\b)x", "barx", 0, false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        pb_arena arena = {0};
        pb_regex *re = compile_ok(&arena, sl(cases[i].pat));
        pb_regex_span sp[2];
        bool got = t_search(re, sl(cases[i].text), cases[i].from, sp, 2);
        if (got != cases[i].match) fprintf(stderr, "assertion case failed: /%s/ on \"%s\" from %zu\n", cases[i].pat, cases[i].text, cases[i].from);
        CHECK(got == cases[i].match);
        pb_arena_free(&arena);
    }
}

// ---------- work budget ----------

// Mirrors the re-replace loop: every search shares one budget.
static pb_regex_result replace_all_scan(pb_regex *re, pb_slice text, size_t *budget, size_t *hits) {
    size_t from = 0;
    *hits = 0;
    while (from <= text.len) {
        pb_regex_span span;
        pb_regex_result r = pb_regex_search(re, text, from, &span, 1, budget);
        if (r != PB_REGEX_MATCH) return r;
        *hits += 1;
        from = span.end > span.start ? span.end : span.end + 1;
    }
    return PB_REGEX_NO_MATCH;
}

static pb_regex *compile_plain(pb_arena *arena, const char *pat) {
    pb_regex *re = NULL;
    CHECK(pb_regex_compile(arena, sl(pat), &re, NULL, NULL, 0) == PB_REGEX_OK);
    return re;
}

static void fill_text(char *buf, size_t n, const char *unit, const char *tail) {
    size_t ul = strlen(unit), tl = strlen(tail);
    for (size_t i = 0; i < n - tl; i += 1) buf[i] = unit[i % ul];
    memcpy(buf + n - tl, tail, tl);
}

static void test_budget(void) {
    pb_arena arena = {0};
    size_t hits = 0;

    // Leftmost-first patterns whose every search scans to the end of the run: quadratic replace-all.
    enum { RUN = 64 * 1024 };
    static char run[RUN];
    static const struct { const char *pat; char ch; } quad[] = {{"a+b|a", 'a'}, {"\\d+%|\\d", '5'}};
    for (size_t i = 0; i < 2; i += 1) {
        memset(run, quad[i].ch, RUN);
        pb_regex *re = compile_plain(&arena, quad[i].pat);
        size_t budget = PB_REGEX_DEFAULT_BUDGET;
        clock_t start = clock();
        CHECK(replace_all_scan(re, (pb_slice){run, RUN}, &budget, &hits) == PB_REGEX_GAVE_UP);
        if (getenv("MB_TEST_EMULATED") == NULL) {
            CHECK(seconds_since(start) < PERF_LIMIT_SECONDS);
        }
        CHECK(budget == 0 && hits > 0);
        pb_regex_span sp[1];
        CHECK(pb_regex_search(re, (pb_slice){run, 100}, 0, sp, 1, NULL) == PB_REGEX_MATCH && sp[0].end == 1);
    }

    enum { MB = 1024 * 1024 };
    static char big[MB];
    memset(big, 'a', MB);
    pb_regex *large = compile_plain(&arena, "(?:a|b)*a(?:a|b){100}c");
    CHECK(!pb_regex_has_dfa(large));
    size_t budget = PB_REGEX_DEFAULT_BUDGET;
    clock_t start = clock();
    CHECK(pb_regex_test(large, (pb_slice){big, MB}, &budget) == PB_REGEX_GAVE_UP);
    pb_regex_span sp[2];
    budget = PB_REGEX_DEFAULT_BUDGET;
    CHECK(pb_regex_search(large, (pb_slice){big, MB}, 0, sp, 2, &budget) == PB_REGEX_GAVE_UP);
    if (getenv("MB_TEST_EMULATED") == NULL) {
            CHECK(seconds_since(start) < PERF_LIMIT_SECONDS);
        }

    static const struct { const char *pat; const char *unit; const char *tail; } real[] = {
        {"\\b\\d{3}-\\d{2}-\\d{4}\\b", "GET /index.html?id=17&q=hello+world HTTP/1.1 agent=curl 12-34 ok\n", " ssn 123-45-6789"},
        {"(?i)<script|drop\\s+table|\\$\\{jndi:", "GET /index.html?id=17&q=hello+world HTTP/1.1 agent=curl 12-34 ok\n", " ${jndi:ldap://x}"},
        {"user=(\\w+)", "GET /index.html?id=17&q=hello+world HTTP/1.1 agent=curl 12-34 ok\n", " user=bob"},
    };
    static char doc[MB];
    for (size_t i = 0; i < 3; i += 1) {
        fill_text(doc, MB, real[i].unit, real[i].tail);
        for (int pike = 0; pike < 2; pike += 1) {
            pb_regex *re = compile_plain(&arena, real[i].pat);
            if (pike) pb_regex_disable_dfa(re);
            budget = PB_REGEX_DEFAULT_BUDGET;
            CHECK(pb_regex_test(re, (pb_slice){doc, MB}, &budget) == PB_REGEX_MATCH);
            budget = PB_REGEX_DEFAULT_BUDGET;
            CHECK(pb_regex_search(re, (pb_slice){doc, MB}, 0, sp, 2, &budget) == PB_REGEX_MATCH);
            CHECK(sp[0].end <= MB && sp[0].end > MB - 32);
            CHECK(budget > 0 && budget < PB_REGEX_DEFAULT_BUDGET);
        }
    }

    // The budget is shared across calls: each identical search spends the same amount.
    pb_regex *twin = compile_plain(&arena, "a+b");
    pb_regex_disable_dfa(twin);
    pb_slice text = sl("xxaaaaab");
    size_t one = 1000000, two = 1000000;
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &one) == PB_REGEX_MATCH);
    size_t used = 1000000 - one;
    CHECK(used > 0);
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &two) == PB_REGEX_MATCH);
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &two) == PB_REGEX_MATCH);
    CHECK(1000000 - two == 2 * used);
    // Exactly enough for one search and one short of two.
    size_t tight = used;
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &tight) == PB_REGEX_MATCH && tight == 0);
    tight = 2 * used - 1;
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &tight) == PB_REGEX_MATCH);
    CHECK(pb_regex_search(twin, text, 0, sp, 2, &tight) == PB_REGEX_GAVE_UP && tight == 0);

    // A DFA rejection consumes nothing, even from a zero budget.
    pb_regex *dfa = compile_plain(&arena, "a+b");
    CHECK(pb_regex_has_dfa(dfa));
    budget = 0;
    CHECK(pb_regex_test(dfa, sl("xxaaaa"), &budget) == PB_REGEX_NO_MATCH && budget == 0);
    CHECK(pb_regex_search(dfa, sl("xxaaaa"), 0, sp, 2, &budget) == PB_REGEX_NO_MATCH && budget == 0);
    CHECK(pb_regex_test(dfa, sl("xxaab"), &budget) == PB_REGEX_MATCH && budget == 0);
    pb_regex_disable_dfa(dfa);
    CHECK(pb_regex_test(dfa, sl("xxaaaa"), &budget) == PB_REGEX_GAVE_UP);
    CHECK(pb_regex_test(dfa, (pb_slice){big, MB}, NULL) == PB_REGEX_NO_MATCH);
    pb_arena_free(&arena);
}

// Test mode stops at the first accepting thread instead of draining higher-priority ones.
static void test_test_mode_early_exit(void) {
    enum { MB = 1024 * 1024 };
    static char as[MB];
    memset(as, 'a', MB);
    pb_arena arena = {0};
    static const char *pats[] = {"(?s)a.*", "(?:a|b)*a(?:a|b){12}"};
    for (size_t i = 0; i < 2; i += 1) {
        pb_regex *re = compile_plain(&arena, pats[i]);
        pb_regex_disable_dfa(re);
        size_t budget = 2000;
        CHECK(pb_regex_test(re, (pb_slice){as, MB}, &budget) == PB_REGEX_MATCH);
        CHECK(budget > 0);
        budget = 2000;
        CHECK(pb_regex_search(re, (pb_slice){as, MB}, 0, NULL, 0, &budget) == PB_REGEX_MATCH);
        budget = 2000;
        // Span mode must still run to the leftmost-first end, so a tiny budget gives up there.
        pb_regex_span sp[1];
        if (i == 0) CHECK(pb_regex_search(re, (pb_slice){as, MB}, 0, sp, 1, &budget) == PB_REGEX_GAVE_UP);
    }
    pb_arena_free(&arena);
}

static bool message_has(const char *pat, const char *needle) {
    pb_arena arena = {0};
    pb_regex *re = NULL;
    char msg[128] = "";
    CHECK(pb_regex_compile(&arena, sl(pat), &re, NULL, msg, sizeof msg) == PB_REGEX_SYNTAX);
    pb_arena_free(&arena);
    return strstr(msg, needle) != NULL;
}

static void test_brace_hint(void) {
    CHECK(message_has("{\"user\":\"(\\w+)\"", "write \\{ for a literal brace"));
    CHECK(message_has("a{x}", "write \\{ for a literal brace"));
    CHECK(message_has("a{2", "write \\{ for a literal brace"));
    CHECK(message_has("x|{", "write \\{ for a literal brace"));
    pb_arena arena = {0};
    pb_regex *re = compile_ok(&arena, sl("\\{\"user\":\"(\\w+)\""));
    pb_regex_span sp[2];
    CHECK(t_search(re, sl("{\"user\":\"bob\"}"), 0, sp, 2) && sp[1].start == 9 && sp[1].end == 12);
    pb_arena_free(&arena);
}

// A failed compile leaves almost nothing in the caller's arena (the parse tree lives in a scratch arena).
static void test_failed_compile_footprint(void) {
    pb_arena arena = {0};
    pb_regex *re = NULL;
    for (int i = 0; i < 200; i += 1) {
        CHECK(pb_regex_compile(&arena, sl("[a-z]+[0-9]+(?:x|y|z)*[^q]{3,9}\\b("), &re, NULL, NULL, 0) == PB_REGEX_SYNTAX);
    }
    CHECK(arena.head == NULL);
    pb_arena_free(&arena);
}

// Pattern families whose DFA exceeds the caps: {n,m} up to 255 on a few hundred bytes.
static void test_dfa_cap_fallback_stress(void) {
    rng r = {0xFEEDFACE12345678ULL};
    size_t fallbacks = 0, with_dfa = 0;
    for (int iter = 0; iter < 300; iter += 1) {
        char pat[96];
        unsigned n = rng_below(&r, 256), m = n + rng_below(&r, 256 - n);
        unsigned kind = rng_below(&r, 4);
        switch (kind) {
        case 0: snprintf(pat, sizeof pat, "[ab]*a[ab]{%u}", n % 40 + 1); break;
        case 1: snprintf(pat, sizeof pat, "[ab]{%u,%u}c", n, m); break;
        case 2: snprintf(pat, sizeof pat, "(?:a|ab){%u,%u}b?c", n % 120, m % 120 + n % 120); break;
        default: snprintf(pat, sizeof pat, "\\b[a-c]{%u,%u}(?:b|c+)$", n, m); break;
        }
        pb_arena arena = {0};
        pb_regex *re = NULL, *pike = NULL;
        if (pb_regex_compile(&arena, sl(pat), &re, NULL, NULL, 0) != PB_REGEX_OK) {
            pb_arena_free(&arena);
            continue;
        }
        CHECK(pb_regex_compile(&arena, sl(pat), &pike, NULL, NULL, 0) == PB_REGEX_OK);
        pb_regex_disable_dfa(pike);
        if (pb_regex_has_dfa(re)) with_dfa += 1; else fallbacks += 1;
        for (int t = 0; t < 4; t += 1) {
            char text[320];
            size_t len = 150 + rng_below(&r, 170);
            for (size_t i = 0; i < len; i += 1) text[i] = "aabbc"[rng_below(&r, 5)];
            pb_slice ts = {text, len};
            size_t from = rng_below(&r, (unsigned)len);
            pb_regex_span a[1], b[1];
            bool hit = t_search(re, ts, from, a, 1);
            CHECK(t_search(pike, ts, from, b, 1) == hit);
            if (hit) CHECK(a[0].start == b[0].start && a[0].end == b[0].end);
            CHECK(t_test(re, ts) == t_test(pike, ts));
            CHECK(t_test(re, ts) == t_search(re, ts, 0, NULL, 0));
        }
        pb_arena_free(&arena);
    }
    CHECK(fallbacks > 20 && with_dfa > 20);

    // Exact reference for the exponential family (a|b)*a(a|b){N}: some 'a' at index <= len-N-1.
    pb_arena arena = {0};
    for (unsigned n = 8; n <= 200; n += 24) {
        char pat[64];
        snprintf(pat, sizeof pat, "(?:a|b)*a(?:a|b){%u}", n);
        pb_regex *re = compile_plain(&arena, pat);
        CHECK(!pb_regex_has_dfa(re));
        for (int t = 0; t < 20; t += 1) {
            char text[400];
            size_t len = rng_below(&r, 390);
            for (size_t i = 0; i < len; i += 1) text[i] = "ab"[rng_below(&r, 2)];
            bool want = false;
            for (size_t i = 0; len >= n + 1 && i + n + 1 <= len; i += 1) want = want || text[i] == 'a';
            CHECK(t_test(re, (pb_slice){text, len}) == want);
        }
    }
    pb_arena_free(&arena);
}

// The visited-generation counter wraps: results near UINT32_MAX equal a fresh regex's, for every `from`.
static void test_generation_wrap(void) {
    static const char *pats[] = {"a+b|a", "(a|ab)(c|bcd)(d*)", "\\bfoo\\w*", "(?:x*y)+z", "(a*)*$"};
    const char *text = "aab abcd foobar xxyxyz aaaa abcdd foo";
    size_t len = strlen(text);
    pb_arena arena = {0};
    for (size_t p = 0; p < sizeof pats / sizeof pats[0]; p += 1) {
        pb_regex *fresh = compile_plain(&arena, pats[p]);
        pb_regex *wrap = compile_plain(&arena, pats[p]);
        pb_regex_disable_dfa(fresh);
        pb_regex_disable_dfa(wrap);
        for (uint32_t back = 1; back < 40; back += 3) {
            pb_regex_set_generation(wrap, UINT32_MAX - back);
            for (size_t from = 0; from <= len; from += 1) {
                pb_regex_span x[4], y[4];
                bool hx = t_search(fresh, sl(text), from, x, 4);
                bool hy = t_search(wrap, sl(text), from, y, 4);
                CHECK(hx == hy);
                for (size_t g = 0; hx && g <= pb_regex_group_count(fresh) && g < 4; g += 1) {
                    CHECK(x[g].matched == y[g].matched && x[g].start == y[g].start && x[g].end == y[g].end);
                }
                CHECK(t_test(fresh, sl(text)) == t_test(wrap, sl(text)));
            }
        }
    }
    pb_arena_free(&arena);
}


// ---------- window start and first-byte table ----------

// One regex per optimization setting, all compiled from the same pattern.
typedef struct opt_set {
    pb_regex *full;      // DFA, window start and first-byte table on
    pb_regex *off;       // DFA, both optimizations off
    pb_regex *window;    // window start only
    pb_regex *first;     // first-byte table only
    pb_regex *pike;      // no DFA, first-byte table on
    pb_regex *pike_off;  // no DFA, first-byte table off
} opt_set;

static bool opt_set_compile(pb_arena *arena, opt_set *o, pb_slice pat) {
    pb_regex **all[] = {&o->full, &o->off, &o->window, &o->first, &o->pike, &o->pike_off};
    for (size_t i = 0; i < 6; i += 1) {
        if (pb_regex_compile(arena, pat, all[i], NULL, NULL, 0) != PB_REGEX_OK) return false;
    }
    pb_regex_disable_window(o->off);
    pb_regex_disable_first_set(o->off);
    pb_regex_disable_first_set(o->window);
    pb_regex_disable_window(o->first);
    pb_regex_disable_dfa(o->pike);
    pb_regex_disable_dfa(o->pike_off);
    pb_regex_disable_first_set(o->pike_off);
    return true;
}

// Compares spans for one (text, from) across every setting; returns whether it matched.
static bool opt_compare(const opt_set *o, const char *label, pb_slice text, size_t from, pb_regex_span *out) {
    pb_regex *others[] = {o->full, o->window, o->first, o->pike, o->pike_off};
    size_t ng = pb_regex_group_count(o->off);
    pb_regex_span ref[10];
    bool hit = t_search(o->off, text, from, ref, 10);
    for (size_t i = 0; i < 5; i += 1) {
        pb_regex_span got[10];
        bool h = t_search(others[i], text, from, got, 10);
        bool same = h == hit;
        for (size_t g = 0; same && hit && g <= ng; g += 1) {
            same = got[g].matched == ref[g].matched && got[g].start == ref[g].start && got[g].end == ref[g].end;
        }
        if (!same) {
            fprintf(stderr, "optimization mismatch (variant %zu): /%.*s/ on len %zu from %zu: off=%d got=%d\n", i, (int)pb_regex_source(o->off).len, pb_regex_source(o->off).ptr, text.len, from, hit, h);
        }
        CHECK(same);
    }
    CHECK(t_test(o->full, text) == t_search(o->off, text, 0, NULL, 0));
    (void)label;
    if (hit && out != NULL) *out = ref[0];
    return hit;
}

static void test_window_and_first_set_directed(void) {
    static char longbuf[3000];
    memset(longbuf, 'x', sizeof longbuf - 1);
    memcpy(longbuf + 2900, " bob@example.com", 16);
    static char ab255[300];
    memset(ab255, 'a', sizeof ab255);
    ab255[259] = 'b';
    static const struct { const char *pat; const char *text; size_t from; int ms; int me; } cases[] = {
        // match straddles `from`: starting inside it is not allowed
        {"abc", "xxabcxx", 3, -1, 0},
        {"abc", "xxabcxx", 2, 2, 5},
        {"[a-c]{3}", "abcabcd", 3, 3, 6},
        {"[a-c]{3}", "abcabcd", 1, 1, 4},
        // from inside the would-be window
        {"a{1,4}b", "aaaaab", 1, 1, 6},
        {"a{1,4}b", "aaaaab", 2, 2, 6},
        // maximum-length match exactly at the window boundary
        {"a{3}", "xaaa", 0, 1, 4},
        {"a{1,3}b", "xxaaab", 0, 2, 6},
        {"a{1,3}b", "xaaaab", 0, 2, 6},
        // zero-count repetition has maximum length 0
        {"a{0}b", "ab", 0, 1, 2},
        {"(?:ab){0}c", "abc", 0, 2, 3},
        // empty match at from, bounded pattern, text long
        {"a?", "bbbbbbbb", 5, 5, 5},
        {"x?", "yyyyyyyyyyyy", 0, 0, 0},
        {"(?:ab)?", "zzzzabzz", 1, 1, 1},
        // anchors and word boundaries at the window edge
        {"\\bab", "zab ab", 0, 4, 6},
        {"\\Bab", "zab", 0, 1, 3},
        {"\\bab\\b", "xab ab", 0, 4, 6},
        {"^ab|b", "xxab", 0, 3, 4},
        {"^ab|b", "abxxab", 0, 0, 2},
        {"a{1,3}$", "xxaaa", 0, 2, 5},
        {"a{1,3}$", "xxaaaa", 0, 3, 6},
        {"$", "abc", 0, 3, 3},
        {"^a?", "bbb", 0, 0, 0},
        {"^a", "ba", 1, -1, 0},
        // lazy quantifiers and alternations with different branch lengths
        {"a{1,3}?", "baaa", 0, 1, 2},
        {"a{2,4}?b", "aaaab", 0, 0, 5},
        {"ab|abcd|x", "zzabcd", 0, 2, 4},
        {"abcd|ab|x", "zzabcd", 0, 2, 6},
        {"x|xyz", "qxyz", 0, 1, 2},
        // counted repetition near 255
        {"a{250,255}b", ab255, 0, 4, 260},
        {"a{255}", ab255, 0, 0, 255},
        {"[a-b]{1,255}b", ab255, 0, 4, 260},
        // JSON-ish and email-ish shapes on a long text
        {"\\b\\w{1,32}@example\\.com", longbuf, 0, 2901, 2916},
        {"\\w{1,32}@example\\.com", longbuf, 100, 2901, 2916},
        // first-byte table: (?i), classes, negated classes, high bytes, alternation, optional leading atoms
        {"(?i)abc", "xxxABc", 0, 3, 6},
        {"(?i)[a-c]x", "zzzzCX", 0, 4, 6},
        {"[^a]b", "aaab\xff" "b", 0, 4, 6},
        {"[^a]b", "aaa\xff" "b", 0, 3, 5},
        {"[\\x80-\\xff]+z", "abc\xc3\xa9z", 0, 3, 6},
        {"[^\\x00-\\x7f]", "abc\xc3", 0, 3, 4},
        {"foo|bar|baz", "xxxxxbaz", 0, 5, 8},
        {"a?b", "xxxb", 0, 3, 4},
        {"a?b", "xxaab", 0, 3, 5},
        {"a*b", "xxxb", 0, 3, 4},
        {"(?:a|b)?c", "xxxc", 0, 3, 4},
        {"a*", "bbb", 1, 1, 1},
        {"\\bfoo", "xfoo foo", 0, 5, 8},
        {"\\b", "...", 0, -1, 0},
        {"\\b", "..a", 0, 2, 2},
        {"^b", "ab", 0, -1, 0},
        {"^b|c", "abc", 0, 2, 3},
        {"x$", "ax", 0, 1, 2},
        {"\\n\\x00", "ab\n\0c", 0, -1, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i += 1) {
        pb_arena arena = {0};
        opt_set o;
        CHECK(opt_set_compile(&arena, &o, sl(cases[i].pat)));
        size_t tl = cases[i].text == ab255 ? sizeof ab255 : (cases[i].text == longbuf ? sizeof longbuf - 1 : strlen(cases[i].text));
        pb_regex_span sp = {0};
        bool hit = opt_compare(&o, cases[i].pat, (pb_slice){cases[i].text, tl}, cases[i].from, &sp);
        bool ok = hit == (cases[i].ms >= 0) && (!hit || ((int)sp.start == cases[i].ms && (int)sp.end == cases[i].me));
        if (!ok) fprintf(stderr, "directed case failed: /%s/ from %zu: hit=%d %zu..%zu\n", cases[i].pat, cases[i].from, hit, sp.start, sp.end);
        CHECK(ok);
        pb_arena_free(&arena);
    }
    // The table is only used where it is sound: nullable and anchored patterns must not skip.
    pb_arena arena = {0};
    pb_regex *re = compile_plain(&arena, "a*");
    pb_regex_disable_dfa(re);
    pb_regex_span sp[1];
    CHECK(t_search(re, sl("bbb"), 0, sp, 1) && sp[0].start == 0 && sp[0].end == 0);
    pb_arena_free(&arena);
}

// Generator for the optimization comparison: bounded and unbounded shapes, assertions anywhere, atoms from a tiny alphabet.
static void gen_opt_pattern(rng *r, gen_buf *g, int depth) {
    static const char *atoms[] = {"a", "b", "c", "x", ".", "[ab]", "[^a]", "\\d", "\\w", "\\b", "\\B", "^", "$", "(?i:a)", "[\\x80-\\xff]", "\\s"};
    static const char *quants[] = {"", "", "", "", "?", "{2}", "{1,3}", "{0,2}", "{2,4}", "??", "{1,3}?", "*", "+", "*?"};
    unsigned items = 1 + rng_below(r, 4);
    for (unsigned i = 0; i < items; i += 1) {
        if (depth < 2 && rng_below(r, 6) == 0) {
            gen_put(g, rng_below(r, 2) ? "(" : "(?:");
            unsigned branches = 1 + rng_below(r, 3);
            for (unsigned b = 0; b < branches; b += 1) {
                if (b > 0) gen_put(g, "|");
                gen_opt_pattern(r, g, depth + 1);
            }
            gen_put(g, ")");
        } else {
            gen_put(g, atoms[rng_below(r, sizeof atoms / sizeof atoms[0])]);
        }
        gen_put(g, quants[rng_below(r, sizeof quants / sizeof quants[0])]);
    }
}

// Optimized spans must equal unoptimized spans for every `from`, short and long texts.
static void test_optimizations_agree(void) {
    rng r = {0xA5A5123456789ABCULL};
    size_t compared = 0, firstset = 0, hits = 0;
    static char text[3200];
    for (int iter = 0; iter < 1500; iter += 1) {
        gen_buf g = {{0}, 0};
        if (rng_below(&r, 4) == 0) gen_put(&g, "(?i)");
        if (rng_below(&r, 10) == 0) gen_put(&g, "^");
        gen_opt_pattern(&r, &g, 0);
        if (rng_below(&r, 8) == 0) gen_put(&g, "$");
        g.s[g.n] = '\0';
        pb_arena arena = {0};
        opt_set o;
        if (!opt_set_compile(&arena, &o, (pb_slice){g.s, g.n})) {
            pb_arena_free(&arena);
            continue;
        }
        firstset += 1;
        for (int t = 0; t < 4; t += 1) {
            size_t len = t < 2 ? rng_below(&r, 40) : 200 + rng_below(&r, t == 2 ? 600 : 2400);
            bool sparse = t >= 2 && rng_below(&r, 3) != 0;
            for (size_t i = 0; i < len; i += 1) {
                if (sparse) {
                    text[i] = rng_below(&r, 24) == 0 ? "abc1 "[rng_below(&r, 5)] : "xx_.\n"[rng_below(&r, 5)];
                } else {
                    unsigned c = rng_below(&r, 11);
                    text[i] = c < 8 ? "abcx1 _."[c] : (char)(c == 8 ? 0xC3 : (c == 9 ? '\n' : 'B'));
                }
            }
            pb_slice ts = {text, len};
            if (len <= 160) {
                for (size_t from = 0; from <= len; from += 1) hits += opt_compare(&o, g.s, ts, from, NULL) ? 1 : 0;
            } else {
                for (int k = 0; k < 48; k += 1) {
                    size_t from = k == 0 ? 0 : (k == 1 ? len : (k == 2 ? len - 1 : rng_below(&r, (unsigned)len + 1)));
                    hits += opt_compare(&o, g.s, ts, from, NULL) ? 1 : 0;
                }
            }
            compared += 1;
        }
        pb_arena_free(&arena);
    }
    printf("optimizations: %zu texts compared across 6 settings (%zu patterns, %zu hits)\n", compared, firstset, hits);
    CHECK(firstset > 400 && hits > 5000);
}


static void run_suite(bool force_pike) {
    g_force_pike = force_pike;
    void (*tests[])(void) = {test_table, test_empty_loops, test_binary, test_from_offsets, test_errors, test_caps, test_pattern_copied, test_linearity, test_fast_paths, test_dfa_assertions};
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i += 1) {
        tests[i]();
    }
}

int main(void) {
    run_suite(false); // DFA fast path where eligible
    run_suite(true);  // Pike VM only
    g_force_pike = false;
    test_dfa_caps();
    test_budget();
    test_test_mode_early_exit();
    test_brace_hint();
    test_failed_compile_footprint();
    test_dfa_cap_fallback_stress();
    test_generation_wrap();
    test_window_and_first_set_directed();
    test_optimizations_agree();
    test_differential();
    test_random_stress();
    puts("regex tests passed");
    return 0;
}
