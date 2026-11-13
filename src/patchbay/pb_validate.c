#include "pb_validate.h"

#include "fs.h"
#include "pb_config.h"
#include "pb_eval.h"
#include "pb_program.h"
#include "pb_json.h"
#include "pb_regex.h"
#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool discard_publish(void *ctx, pb_slice subject, pb_slice payload) {
    (void)ctx;
    (void)payload;
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    return mb_proto_subject_valid(mb_subject, false) &&
           !mb_router_subject_has_lvc_prefix(mb_subject) &&
           !mb_router_subject_has_stats_prefix(mb_subject);
}

// Throwaway config targets so --validate runs the same loaders as startup.
typedef struct validate_config {
    pb_config_ctx ctx;
    pb_lvc_config lvc;
    pb_bridge_config bridge;
    pb_imports_config imports;
} validate_config;

// Sample-evaluates one rule body once, as the runtime would for a publish (or,
// for every rules, a clock fire with empty subject/payload and no reply subject).
static bool validate_sample(size_t rule_id, pb_value body, bool every) {
    pb_arena scratch = {0};
    pb_eval_state state = {0};
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = &state,
        .rule_id = rule_id,
        .subject = every ? (pb_slice){.ptr = "", .len = 0} : (pb_slice){.ptr = "x.y.z", .len = 5},
        .payload = every ? (pb_slice){.ptr = "", .len = 0} : (pb_slice){.ptr = "1", .len = 1},
        .reply_to = every ? (pb_slice){.ptr = "", .len = 0} : (pb_slice){.ptr = "_INBOX.validate", .len = 15},
        .publish = discard_publish,
    };
    const pb_eval_result r = pb_eval(&ctx, body);
    pb_eval_state_free(&state);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "validate: rule %zu eval failed: %s\n", rule_id, pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

// Notes each regex literal without a DFA: it runs on the slower Pike VM and can
// trip the work budget. Not an error.
static void note_slow_patterns(size_t rule_id, const pb_value *v) {
    if (v->kind != PB_LIST && v->kind != PB_VECTOR) {
        return;
    }
    for (size_t i = 0; i < v->seq.len; i += 1) {
        const pb_value *item = &v->seq.items[i];
        if (item->kind == PB_REGEX && !pb_regex_has_dfa(item->regex)) {
            const pb_slice form = v->kind == PB_LIST && i > 0 ? v->seq.items[0].text : (pb_slice){0};
            const pb_slice src = pb_regex_source(item->regex);
            fprintf(stderr, "note: rule %zu: %.*s: pattern runs without the fast table (%zu instructions): \"%.*s\"\n", rule_id,
                    (int)form.len, form.ptr, pb_regex_program_len(item->regex), (int)src.len, src.ptr);
        } else {
            note_slow_patterns(rule_id, item);
        }
    }
}

static bool validate_on_form(validate_config *config, pb_value form, size_t *rule_count) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "validate: top-level form must be a list headed by a symbol\n");
        return false;
    }

    pb_values items = form.seq;
    if (pb_config_head_is(items.items[0].text)) {
        return pb_config_load_form(&config->ctx, items, &config->lvc, &config->bridge, &config->imports);
    }
    if (pb_slice_eq_lit(items.items[0].text, "every")) {
        *rule_count += 1;
        pb_every_spec spec = {0};
        char every_err[160];
        if (!pb_every_parse(config->ctx.arena, items, false, false, &spec, every_err, sizeof every_err)) {
            fprintf(stderr, "validate: rule %zu (every): %s\n", *rule_count - 1, every_err);
            return false;
        }
        return validate_sample(*rule_count - 1, *spec.body, true);
    }
    if (!pb_slice_eq_lit(items.items[0].text, "on")) {
        return true;
    }
    *rule_count += 1;

    if (items.len < 3 || items.items[1].kind != PB_STRING) {
        fprintf(stderr, "validate: invalid on form\n");
        return false;
    }

    size_t body_idx = items.len - 1;
    size_t i = 2;
    while (i < body_idx) {
        if (i + 1 >= body_idx || items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "validate: invalid on options\n");
            return false;
        }
        if (!pb_slice_eq_lit(items.items[i].text, "reentrant")) {
            fprintf(stderr, "validate: unknown on option: %.*s\n", (int)items.items[i].text.len,
                    items.items[i].text.ptr);
            return false;
        }
        if (items.items[i + 1].kind != PB_BOOL) {
            fprintf(stderr, "validate: :reentrant expects boolean\n");
            return false;
        }
        i += 2;
    }

    // Resolve first so every branch of the body is checked for unknown forms,
    // not only the path the sample evaluation below happens to take.
    char resolve_err[160];
    if (!pb_resolve_rule(config->ctx.arena, &items.items[body_idx], false, false, resolve_err, sizeof resolve_err)) {
        fprintf(stderr, "validate: rule %zu (\"%.*s\"): %s\n", *rule_count - 1,
                (int)items.items[1].text.len, items.items[1].text.ptr, resolve_err);
        return false;
    }

    note_slow_patterns(*rule_count - 1, &items.items[body_idx]);
    return validate_sample(*rule_count - 1, items.items[body_idx], false);
}

int pb_validate_file(const char *path) {
    int rc = 1;
    mb_buf source = {0};
    pb_arena arena = {0};
    if (!mb_read_file(path, &source)) {
        perror(path);
        goto cleanup;
    }

    const pb_parse_result parsed = pb_parse_patchbay_source(&arena, path, (const char *)source.ptr, source.len);
    if (parsed.err != PB_PARSE_OK) {
        size_t line = 0;
        size_t col = 0;
        pb_line_col((const char *)source.ptr, source.len, parsed.err_offset, &line, &col);
        fprintf(stderr, "validate: parse error: %s%s%s at %zu:%zu (byte %zu)\n", pb_parse_error_name(parsed.err),
                parsed.err_msg[0] != '\0' ? ": " : "", parsed.err_msg, line, col, parsed.err_offset);
        goto cleanup;
    }

    bool ok = true;
    size_t rules = 0;
    validate_config config = {.ctx = {.arena = &arena, .prefix = "validate"}};
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!validate_on_form(&config, parsed.forms.items[i], &rules)) {
            ok = false;
        }
    }
    pb_config_free(&config.lvc, &config.bridge, &config.imports);

    if (ok) {
        printf("%s: ok (%zu rule%s)\n", path, rules, rules == 1 ? "" : "s");
    }

    rc = ok ? 0 : 1;
cleanup:
    pb_arena_free(&arena);
    mb_buf_free(&source);
    return rc;
}
