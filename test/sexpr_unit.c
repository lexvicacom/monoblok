#include "pb_sexpr.h"
#include "test_check.h"
#include "test_pb_check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pb_parse_result parse(pb_arena *arena, const char *src) {
    return pb_parse_all(arena, src, strlen(src));
}

static size_t arena_retained_cap(const pb_arena *arena) {
    size_t total = 0;
    for (const pb_arena_block *b = arena->head; b != NULL; b = b->next) {
        total += b->cap;
    }
    return total;
}

static void test_arena_trim(void) {
    pb_arena arena = {0};
    CHECK(pb_arena_alloc(&arena, 32, _Alignof(double)) != NULL);
    CHECK(pb_arena_alloc(&arena, 128 * 1024, 1) != NULL);

    pb_arena_reset(&arena);
    pb_arena_trim(&arena, 4096);
    CHECK(arena.head != NULL);
    CHECK(arena_retained_cap(&arena) <= 4096);

    CHECK(pb_arena_alloc(&arena, 16, _Alignof(double)) != NULL);
    pb_arena_trim(&arena, 0);
    CHECK(arena.head != NULL);

    pb_arena_reset(&arena);
    pb_arena_trim(&arena, 0);
    CHECK(arena.head == NULL);
    pb_arena_free(&arena);
}

static void test_atoms(void) {
    pb_arena arena = {0};
    pb_parse_result r = parse(&arena, "42 -3.5 foo \"bar\" true false nil");
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 7);
    CHECK(r.forms.items[0].kind == PB_NUMBER);
    CHECK(r.forms.items[0].number == 42.0);
    CHECK(r.forms.items[1].kind == PB_NUMBER);
    CHECK(fabs(r.forms.items[1].number - -3.5) < 0.00001);
    CHECK(r.forms.items[2].kind == PB_SYMBOL);
    check_text(r.forms.items[2].text, "foo");
    CHECK(r.forms.items[3].kind == PB_STRING);
    check_text(r.forms.items[3].text, "bar");
    CHECK(r.forms.items[4].kind == PB_BOOL && r.forms.items[4].boolean);
    CHECK(r.forms.items[5].kind == PB_BOOL && !r.forms.items[5].boolean);
    CHECK(r.forms.items[6].kind == PB_NIL);
    pb_arena_free(&arena);
}

static void test_keywords(void) {
    pb_arena arena = {0};
    pb_parse_result r = parse(&arena, "(bridge :servers \"a\" :tls true)");
    CHECK(r.err == PB_PARSE_OK);
    pb_values top = r.forms.items[0].seq;
    check_text(top.items[0].text, "bridge");
    CHECK(top.items[1].kind == PB_KEYWORD);
    check_text(top.items[1].text, "servers");
    check_text(top.items[2].text, "a");
    check_text(top.items[3].text, "tls");
    CHECK(top.items[4].boolean);
    pb_arena_free(&arena);
}

static void test_nested_list(void) {
    pb_arena arena = {0};
    pb_parse_result r = parse(&arena, "(on \"foo.*\" (publish! (subject-append \"hi\") payload))");
    CHECK(r.err == PB_PARSE_OK);
    pb_values top = r.forms.items[0].seq;
    check_text(top.items[0].text, "on");
    check_text(top.items[1].text, "foo.*");
    pb_values body = top.items[2].seq;
    check_text(body.items[0].text, "publish!");
    pb_values arg0 = body.items[1].seq;
    check_text(arg0.items[0].text, "subject-append");
    check_text(arg0.items[1].text, "hi");
    check_text(body.items[2].text, "payload");
    pb_arena_free(&arena);
}

static void test_comments_and_escapes(void) {
    pb_arena arena = {0};
    pb_parse_result r = parse(&arena, "; comment\n(a \"b\\n\\\"c\") ; inline\n");
    CHECK(r.err == PB_PARSE_OK);
    CHECK(r.forms.len == 1);
    pb_values top = r.forms.items[0].seq;
    CHECK(top.len == 2);
    check_text(top.items[1].text, "b\n\"c");
    pb_arena_free(&arena);
}

static void test_vectors(void) {
    pb_arena arena = {0};
    pb_parse_result r = parse(&arena, "(contains? [1 2 3] x)");
    CHECK(r.err == PB_PARSE_OK);
    pb_values top = r.forms.items[0].seq;
    check_text(top.items[0].text, "contains?");
    CHECK(top.items[1].kind == PB_VECTOR);
    CHECK(top.items[1].seq.len == 3);
    check_text(top.items[2].text, "x");
    pb_arena_free(&arena);
}

static void test_errors(void) {
    pb_arena arena = {0};
    CHECK(parse(&arena, "(a b").err == PB_PARSE_UNEXPECTED_EOF);
    CHECK(parse(&arena, ")").err == PB_PARSE_UNEXPECTED_RPAREN);
    CHECK(parse(&arena, "]").err == PB_PARSE_UNEXPECTED_RBRACKET);
    CHECK(parse(&arena, "(a b]").err == PB_PARSE_MISMATCHED_BRACKET);
    CHECK(parse(&arena, "[a b)").err == PB_PARSE_MISMATCHED_BRACKET);
    CHECK(parse(&arena, "()").err == PB_PARSE_INVALID_LIST_HEAD);
    CHECK(parse(&arena, "(\"not-call\" 1 2)").err == PB_PARSE_INVALID_LIST_HEAD);
    CHECK(parse(&arena, "(:servers [\"nats://a:4222\"])").err == PB_PARSE_INVALID_LIST_HEAD);
    pb_arena_free(&arena);
}

static void test_offset_at_eof(void) {
    pb_arena arena = {0};
    const char *src = "(on \"foo\"\n  (publish bar baz)\n";
    pb_parse_result r = parse(&arena, src);
    CHECK(r.err == PB_PARSE_UNEXPECTED_EOF);
    CHECK(r.err_offset == strlen(src));
    pb_arena_free(&arena);
}

static void test_result_does_not_borrow_source_atoms(void) {
    char src[] = "(bridge :servers [\"nats://a:4222\"])";
    pb_arena arena = {0};
    pb_parse_result r = pb_parse_all(&arena, src, strlen(src));
    CHECK(r.err == PB_PARSE_OK);

    pb_values top = r.forms.items[0].seq;
    memset(src, 'x', sizeof src - 1);
    check_text(top.items[0].text, "bridge");
    check_text(top.items[1].text, "servers");

    pb_arena_free(&arena);
}

TEST_MAIN(sexpr,
          test_arena_trim,
          test_atoms,
          test_keywords,
          test_nested_list,
          test_comments_and_escapes,
          test_vectors,
          test_errors,
          test_offset_at_eof,
          test_result_does_not_borrow_source_atoms)
