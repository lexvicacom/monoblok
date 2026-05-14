#include "pb_validate.h"

#include "pb_eval.h"
#include "pb_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool slice_eq(pb_slice s, const char *lit) {
    const size_t n = strlen(lit);
    return s.len == n && memcmp(s.ptr, lit, n) == 0;
}

static bool read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return false;
    }
    const long size = ftell(f);
    if (size < 0) {
        perror("ftell");
        fclose(f);
        return false;
    }
    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return false;
    }
    const size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return false;
    }
    buf[nread] = '\0';
    *out = buf;
    *out_len = nread;
    return true;
}

static bool discard_publish(void *ctx, pb_slice subject, pb_slice payload) {
    (void)ctx;
    (void)subject;
    (void)payload;
    return true;
}

static bool validate_on_form(pb_value form, size_t *rule_count) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "validate: top-level form must be a list headed by a symbol\n");
        return false;
    }

    pb_values items = form.seq;
    if (!slice_eq(items.items[0].text, "on")) {
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
        if (!slice_eq(items.items[i].text, "reentrant")) {
            fprintf(stderr, "validate: unknown on option: %.*s\n", (int)items.items[i].text.len, items.items[i].text.ptr);
            return false;
        }
        if (items.items[i + 1].kind != PB_BOOL) {
            fprintf(stderr, "validate: :reentrant expects boolean\n");
            return false;
        }
        i += 2;
    }

    pb_arena scratch = {0};
    pb_eval_state state = {0};
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = &state,
        .rule_id = *rule_count - 1,
        .subject = {.ptr = "x", .len = 1},
        .payload = {.ptr = "1", .len = 1},
        .publish = discard_publish,
    };
    const pb_eval_result r = pb_eval(&ctx, items.items[body_idx]);
    pb_eval_state_free(&state);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "validate: rule eval failed: %s\n", pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

int pb_validate_file(const char *path) {
    char *source = NULL;
    size_t source_len = 0;
    if (!read_file(path, &source, &source_len)) {
        return 1;
    }

    pb_arena arena = {0};
    const pb_parse_result parsed = pb_parse_patchbay_source(&arena, path, source, source_len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "validate: parse error: %s at byte %zu\n",
                pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_arena_free(&arena);
        free(source);
        return 1;
    }

    bool ok = true;
    size_t rules = 0;
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!validate_on_form(parsed.forms.items[i], &rules)) {
            ok = false;
        }
    }

    if (ok) {
        printf("%s: ok (%zu rule%s)\n", path, rules, rules == 1 ? "" : "s");
    }

    pb_arena_free(&arena);
    free(source);
    return ok ? 0 : 1;
}
