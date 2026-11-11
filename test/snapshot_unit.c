#include "snapshot.h"
#include "test_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static mb_slice lit(const char *s) {
    return (mb_slice){.ptr = (const uint8_t *)s, .len = strlen(s)};
}

static bool buf_contains(const mb_buf *buf, const char *needle) {
    const size_t n = strlen(needle);
    for (size_t i = 0; i + n <= buf->len; i += 1) {
        if (memcmp(buf->ptr + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static void configure_router_lvc(mb_router *router, const pb_program *program) {
    mb_slice filters[8];
    CHECK(program->lvc.len <= sizeof filters / sizeof filters[0]);
    for (size_t i = 0; i < program->lvc.len; i += 1) {
        filters[i] = (mb_slice){.ptr = (const uint8_t *)program->lvc.filters[i].ptr,
                                .len = program->lvc.filters[i].len};
    }
    CHECK(mb_router_configure_lvc(router, filters, program->lvc.len));
}

static void test_snapshot_roundtrip_lvc_and_state(void) {
    const char *src =
        "(lvc [\">\"])\n"
        "(on \"foo\" (when (changed? payload) (publish! \"out\" payload)))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-%ld.bin", (long)getpid());
    remove(path);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    configure_router_lvc(&router, &program);
    CHECK(mb_router_publish(&router, lit("foo"), lit("cached")));
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("same"), 0, 0, (pb_program_eval_options){0}));
    CHECK(mb_snapshot_write(path, &router, &program));
    mb_router_free(&router);
    pb_program_free(&program);

    pb_program restored_program = {0};
    CHECK(pb_program_load_source(&restored_program, "<test>", src, strlen(src)));
    mb_router restored_router;
    mb_router_init(&restored_router);
    configure_router_lvc(&restored_router, &restored_program);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &restored_router, &restored_program, &counts));
    CHECK(counts.lvc == 2);
    CHECK(counts.rule_state == 1);

    mb_router_conn lvc_conn = {0};
    CHECK(mb_router_subscribe(&restored_router, &lvc_conn, lit("$LVC.foo"), lit("1")));
    CHECK(buf_contains(&lvc_conn.out, "MSG $LVC.foo 1 6\r\ncached\r\n"));

    mb_router_conn out_conn = {0};
    CHECK(mb_router_subscribe(&restored_router, &out_conn, lit("out"), lit("2")));
    CHECK(pb_program_eval_publish(&restored_program, &restored_router, lit("foo"), lit("same"), 0, 0, (pb_program_eval_options){0}));
    CHECK(out_conn.out.len == 0);
    CHECK(pb_program_eval_publish(&restored_program, &restored_router, lit("foo"), lit("next"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&out_conn.out, "MSG out 2 4\r\nnext\r\n"));

    mb_buf_free(&lvc_conn.out);
    mb_buf_free(&out_conn.out);
    mb_router_free(&restored_router);
    pb_program_free(&restored_program);
    remove(path);
}

static void test_snapshot_preserves_count_window_capacity(void) {
    const char *src = "(on \"foo\" (publish! \"avg\" (moving-avg 3 payload-float)))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-window-%ld.bin", (long)getpid());
    remove(path);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("1"), 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("2"), 0, 0, (pb_program_eval_options){0}));
    CHECK(mb_snapshot_write(path, &router, &program));
    mb_router_free(&router);
    pb_program_free(&program);

    pb_program restored_program = {0};
    CHECK(pb_program_load_source(&restored_program, "<test>", src, strlen(src)));
    mb_router restored_router;
    mb_router_init(&restored_router);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &restored_router, &restored_program, &counts));
    CHECK(counts.rule_state == 1);

    mb_router_conn avg_conn = {0};
    CHECK(mb_router_subscribe(&restored_router, &avg_conn, lit("avg"), lit("1")));
    CHECK(pb_program_eval_publish(&restored_program, &restored_router, lit("foo"), lit("3"), 0, 0, (pb_program_eval_options){0}));
    CHECK(buf_contains(&avg_conn.out, "MSG avg 1 1\r\n2\r\n"));
    CHECK(!buf_contains(&avg_conn.out, "MSG avg 1 3\r\n2.5\r\n"));

    mb_buf_free(&avg_conn.out);
    mb_router_free(&restored_router);
    pb_program_free(&restored_program);
    remove(path);
}

static void put_u32(mb_buf *b, uint32_t v) {
    for (int i = 0; i < 4; i += 1) {
        CHECK(mb_buf_append_byte(b, (uint8_t)(v >> (8 * i))));
    }
}

static void put_lp(mb_buf *b, const char *s) {
    put_u32(b, (uint32_t)strlen(s));
    CHECK(mb_buf_append(b, s, strlen(s)));
}

static void write_bytes_file(const char *path, const mb_buf *b) {
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    CHECK(fwrite(b->ptr, 1, b->len, f) == b->len);
    fclose(f);
}

// A v4 file (no call-site id) must still load its LVC entries but skip rule state.
static void test_snapshot_v4_rule_state_skipped(void) {
    const char *src = "(lvc [\">\"])\n(on \"foo\" (squelch payload))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-v4-%ld.bin", (long)getpid());

    mb_buf file = {0};
    CHECK(mb_buf_append(&file, "MBLK", 4));
    CHECK(mb_buf_append_byte(&file, 4));
    CHECK(mb_buf_append(&file, "\0\0\0", 3));
    CHECK(mb_buf_append_byte(&file, 0x01));
    put_lp(&file, "foo");
    put_lp(&file, "cached");
    CHECK(mb_buf_append_byte(&file, 0x02));
    put_u32(&file, 0);
    put_lp(&file, "foo");
    put_lp(&file, "squelch:foo");
    CHECK(mb_buf_append_byte(&file, 0x02));
    for (int i = 0; i < 8; i += 1) {
        CHECK(mb_buf_append_byte(&file, 0));
    }
    write_bytes_file(path, &file);
    mb_buf_free(&file);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    configure_router_lvc(&router, &program);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &router, &program, &counts));
    CHECK(counts.lvc == 1);
    CHECK(counts.rule_state == 0);
    CHECK(counts.rule_state_skipped == 1);
    CHECK(program.rules[0].state.len == 0);
    mb_router_free(&router);
    pb_program_free(&program);
    remove(path);
}

// State for a call site that an edited rule no longer has must not be restored.
static void test_snapshot_skips_state_for_changed_rule(void) {
    const char *src_a = "(on \"foo\" (squelch payload))\n";
    const char *src_b = "(on \"foo\" (changed? payload))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-edit-%ld.bin", (long)getpid());
    remove(path);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src_a, strlen(src_a)));
    mb_router router;
    mb_router_init(&router);
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("x"), 0, 0, (pb_program_eval_options){0}));
    CHECK(program.rules[0].state.len == 1);
    CHECK(program.rules[0].state.items[0].site != 0);
    CHECK(mb_snapshot_write(path, &router, &program));
    mb_router_free(&router);
    pb_program_free(&program);

    // Same rule: restored.
    pb_program same = {0};
    CHECK(pb_program_load_source(&same, "<test>", src_a, strlen(src_a)));
    mb_router same_router;
    mb_router_init(&same_router);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &same_router, &same, &counts));
    CHECK(counts.rule_state == 1);
    CHECK(counts.rule_state_skipped == 0);
    CHECK(same.rules[0].state.items[0].site != 0);
    mb_router_free(&same_router);
    pb_program_free(&same);

    // Same filter and site number but a different form there: skipped.
    pb_program edited = {0};
    CHECK(pb_program_load_source(&edited, "<test>", src_b, strlen(src_b)));
    mb_router edited_router;
    mb_router_init(&edited_router);
    counts = (mb_snapshot_counts){0};
    CHECK(mb_snapshot_load(path, &edited_router, &edited, &counts));
    CHECK(counts.rule_state == 0);
    CHECK(counts.rule_state_skipped == 1);
    CHECK(edited.rules[0].state.len == 0);
    mb_router_free(&edited_router);
    pb_program_free(&edited);
    remove(path);
}
// every slots are recreated at load and never persisted; state of stateful forms
// inside an every body (recorded under the empty filter) and of on rules restores
// without duplicating the every slot.
static void test_snapshot_every_rules(void) {
    const char *src =
        "(every :ms 100 (publish! \"sum\" (moving-sum 100 1)))\n"
        "(on \"foo\" (when (changed? payload) (publish! \"out\" payload)))\n";
    char path[128];
    snprintf(path, sizeof path, "/tmp/monoblok-snapshot-every-%ld.bin", (long)getpid());
    remove(path);

    pb_program program = {0};
    CHECK(pb_program_load_source(&program, "<test>", src, strlen(src)));
    mb_router router;
    mb_router_init(&router);
    CHECK(pb_program_tick(&program, &router, 0, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_tick(&program, &router, 100, 0, (pb_program_eval_options){0}));
    CHECK(pb_program_eval_publish(&program, &router, lit("foo"), lit("same"), 0, 0, (pb_program_eval_options){0}));
    CHECK(program.rules[0].state.len == 2); // the every slot plus moving-sum
    CHECK(mb_snapshot_write(path, &router, &program));
    mb_router_free(&router);
    pb_program_free(&program);

    pb_program restored = {0};
    CHECK(pb_program_load_source(&restored, "<test>", src, strlen(src)));
    CHECK(restored.rules[0].state.len == 1);
    mb_router restored_router;
    mb_router_init(&restored_router);
    mb_snapshot_counts counts = {0};
    CHECK(mb_snapshot_load(path, &restored_router, &restored, &counts));
    CHECK(counts.rule_state == 2);
    CHECK(counts.rule_state_skipped == 0);
    CHECK(restored.rules[0].state.len == 2);
    size_t every_slots = 0;
    for (size_t i = 0; i < restored.rules[0].state.len; i += 1) {
        const pb_eval_state_entry *e = &restored.rules[0].state.items[i];
        if (e->clock_kind == PB_EVAL_CLOCK_EVERY) {
            every_slots += 1;
            CHECK(e->clock_deadline_ms == 0); // restored programs reschedule from the first tick
        } else {
            CHECK(e->subject_len == 0);
        }
    }
    CHECK(every_slots == 1);
    uint64_t deadline = 1;
    CHECK(pb_program_next_clock_deadline(&restored, &deadline) && deadline == 0);
    mb_router_free(&restored_router);
    pb_program_free(&restored);

    // Swapping the order puts an on rule at the saved every index: filters differ, so state is skipped.
    const char *swapped =
        "(on \"foo\" (when (changed? payload) (publish! \"out\" payload)))\n"
        "(every :ms 100 (publish! \"sum\" (moving-sum 100 1)))\n";
    pb_program edited = {0};
    CHECK(pb_program_load_source(&edited, "<test>", swapped, strlen(swapped)));
    mb_router edited_router;
    mb_router_init(&edited_router);
    mb_snapshot_counts edited_counts = {0};
    CHECK(mb_snapshot_load(path, &edited_router, &edited, &edited_counts));
    CHECK(edited_counts.rule_state == 0);
    CHECK(edited_counts.rule_state_skipped == 2);
    mb_router_free(&edited_router);
    pb_program_free(&edited);
    remove(path);
}

TEST_MAIN(snapshot,
          test_snapshot_roundtrip_lvc_and_state,
          test_snapshot_preserves_count_window_capacity,
          test_snapshot_v4_rule_state_skipped,
          test_snapshot_skips_state_for_changed_rule,
          test_snapshot_every_rules)
