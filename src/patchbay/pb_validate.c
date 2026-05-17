#include "pb_validate.h"

#include "fs.h"
#include "pb_eval.h"
#include "pb_json.h"
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

static bool is_export_config_head(pb_slice head) {
    return pb_slice_eq_lit(head, "bridge") || pb_slice_eq_lit(head, "export");
}

static const char *export_config_name(pb_slice head) {
    return pb_slice_eq_lit(head, "export") ? "export" : "bridge";
}

static bool value_is_env_form(pb_value value) {
    return value.kind == PB_LIST &&
           value.seq.len == 2 &&
           value.seq.items[0].kind == PB_SYMBOL &&
           pb_slice_eq_lit(value.seq.items[0].text, "env");
}

static bool env_value_nonempty(pb_slice name, bool *out) {
    char *name_buf = malloc(name.len + 1);
    if (name_buf == NULL) {
        return false;
    }
    memcpy(name_buf, name.ptr, name.len);
    name_buf[name.len] = '\0';
    const char *value = getenv(name_buf);
    free(name_buf);
    *out = value != NULL && value[0] != '\0';
    return true;
}

static bool validate_config_string(pb_value value, const char *form_name, pb_slice key) {
    if (value.kind == PB_STRING) {
        if (value.text.len != 0) {
            return true;
        }
        fprintf(stderr, "validate: %s :%.*s must not be empty\n", form_name, (int)key.len, key.ptr);
        return false;
    }
    if (value_is_env_form(value)) {
        if (value.seq.items[1].kind != PB_STRING || value.seq.items[1].text.len == 0) {
            fprintf(stderr, "validate: %s :%.*s expects non-empty string or (env \"NAME\")\n", form_name, (int)key.len, key.ptr);
            return false;
        }
        bool nonempty = false;
        if (!env_value_nonempty(value.seq.items[1].text, &nonempty)) {
            return false;
        }
        if (!nonempty) {
            fprintf(stderr, "validate: %s :%.*s must not be empty\n", form_name, (int)key.len, key.ptr);
            return false;
        }
        return true;
    }
    fprintf(stderr, "validate: %s :%.*s expects non-empty string or (env \"NAME\")\n", form_name, (int)key.len, key.ptr);
    return false;
}

static bool validate_config_string_list(pb_value value, const char *form_name, pb_slice key) {
    if (value.kind == PB_STRING || value_is_env_form(value)) {
        return validate_config_string(value, form_name, key);
    }
    if (value.kind != PB_VECTOR && value.kind != PB_LIST) {
        fprintf(stderr, "validate: %s :%.*s expects string, (env \"NAME\"), or vector/list of strings\n", form_name,
                (int)key.len, key.ptr);
        return false;
    }
    if (value.seq.len == 0) {
        fprintf(stderr, "validate: %s :%.*s must not be empty\n", form_name, (int)key.len, key.ptr);
        return false;
    }
    for (size_t i = 0; i < value.seq.len; i += 1) {
        if (!validate_config_string(value.seq.items[i], form_name, key)) {
            return false;
        }
    }
    return true;
}

static bool validate_on_form(pb_value form, size_t *rule_count) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "validate: top-level form must be a list headed by a symbol\n");
        return false;
    }

    pb_values items = form.seq;
    if (pb_slice_eq_lit(items.items[0].text, "lvc")) {
        if (items.len < 2) {
            fprintf(stderr, "validate: lvc expects at least one filter\n");
            return false;
        }
        if (items.len == 2 && items.items[1].kind == PB_VECTOR) {
            pb_values filters = items.items[1].seq;
            if (filters.len == 0) {
                fprintf(stderr, "validate: lvc vector must not be empty\n");
                return false;
            }
            pb_slice key = {.ptr = "filter", .len = sizeof "filter" - 1};
            for (size_t i = 0; i < filters.len; i += 1) {
                if (!validate_config_string(filters.items[i], "lvc", key)) {
                    return false;
                }
            }
            return true;
        }
        pb_slice key = {.ptr = "filter", .len = sizeof "filter" - 1};
        for (size_t i = 1; i < items.len; i += 1) {
            if (!validate_config_string(items.items[i], "lvc", key)) {
                return false;
            }
        }
        return true;
    }
    if (is_export_config_head(items.items[0].text)) {
        const char *form_name = export_config_name(items.items[0].text);
        if (items.len < 3 || (items.len % 2) == 0) {
            fprintf(stderr, "validate: %s expects keyword/value options\n", form_name);
            return false;
        }
        bool has_servers = false;
        for (size_t i = 1; i < items.len; i += 2) {
            if (items.items[i].kind != PB_KEYWORD) {
                fprintf(stderr, "validate: %s option must be keyword\n", form_name);
                return false;
            }
            pb_slice key = items.items[i].text;
            pb_value value = items.items[i + 1];
            if (pb_slice_eq_lit(key, "servers") || pb_slice_eq_lit(key, "export")) {
                if (pb_slice_eq_lit(key, "servers")) {
                    has_servers = true;
                }
                if (!validate_config_string_list(value, form_name, key)) {
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "tls") || pb_slice_eq_lit(key, "tls-skip-verify") ||
                       pb_slice_eq_lit(key, "origin-header")) {
                if (value.kind != PB_BOOL) {
                    fprintf(stderr, "validate: %s :%.*s expects boolean\n", form_name, (int)key.len, key.ptr);
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "connect-timeout-ms") || pb_slice_eq_lit(key, "ping-interval-ms") ||
                       pb_slice_eq_lit(key, "reconnect-wait-ms") || pb_slice_eq_lit(key, "max-reconnect")) {
                if (value.kind != PB_NUMBER) {
                    fprintf(stderr, "validate: %s :%.*s expects number\n", form_name, (int)key.len, key.ptr);
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "name") || pb_slice_eq_lit(key, "creds") || pb_slice_eq_lit(key, "user") ||
                       pb_slice_eq_lit(key, "password") || pb_slice_eq_lit(key, "token") || pb_slice_eq_lit(key, "tls-ca") ||
                       pb_slice_eq_lit(key, "tls-cert") || pb_slice_eq_lit(key, "tls-key")) {
                if (!validate_config_string(value, form_name, key)) {
                    return false;
                }
            } else {
                fprintf(stderr, "validate: unknown %s option: %.*s\n", form_name, (int)key.len, key.ptr);
                return false;
            }
        }
        if (!has_servers) {
            fprintf(stderr, "validate: %s requires :servers\n", form_name);
            return false;
        }
        return true;
    }
    if (pb_slice_eq_lit(items.items[0].text, "import")) {
        if (items.len < 3 || (items.len % 2) == 0) {
            fprintf(stderr, "validate: import expects keyword/value options\n");
            return false;
        }
        bool has_servers = false;
        bool has_subjects = false;
        for (size_t i = 1; i < items.len; i += 2) {
            if (items.items[i].kind != PB_KEYWORD) {
                fprintf(stderr, "validate: import option must be keyword\n");
                return false;
            }
            pb_slice key = items.items[i].text;
            pb_value value = items.items[i + 1];
            if (pb_slice_eq_lit(key, "servers") || pb_slice_eq_lit(key, "subject") || pb_slice_eq_lit(key, "subjects")) {
                if (pb_slice_eq_lit(key, "servers")) {
                    has_servers = true;
                } else {
                    has_subjects = true;
                }
                if (!validate_config_string_list(value, "import", key)) {
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "tls") || pb_slice_eq_lit(key, "tls-skip-verify") ||
                       pb_slice_eq_lit(key, "origin-header")) {
                if (value.kind != PB_BOOL) {
                    fprintf(stderr, "validate: import :%.*s expects boolean\n", (int)key.len, key.ptr);
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "connect-timeout-ms") || pb_slice_eq_lit(key, "ping-interval-ms") ||
                       pb_slice_eq_lit(key, "reconnect-wait-ms") || pb_slice_eq_lit(key, "max-reconnect") ||
                       pb_slice_eq_lit(key, "max-pending")) {
                if (value.kind != PB_NUMBER) {
                    fprintf(stderr, "validate: import :%.*s expects number\n", (int)key.len, key.ptr);
                    return false;
                }
            } else if (pb_slice_eq_lit(key, "name") || pb_slice_eq_lit(key, "creds") || pb_slice_eq_lit(key, "user") ||
                       pb_slice_eq_lit(key, "password") || pb_slice_eq_lit(key, "token") || pb_slice_eq_lit(key, "tls-ca") ||
                       pb_slice_eq_lit(key, "tls-cert") || pb_slice_eq_lit(key, "tls-key")) {
                if (!validate_config_string(value, "import", key)) {
                    return false;
                }
            } else {
                fprintf(stderr, "validate: unknown import option: %.*s\n", (int)key.len, key.ptr);
                return false;
            }
        }
        if (!has_servers) {
            fprintf(stderr, "validate: import requires :servers\n");
            return false;
        }
        if (!has_subjects) {
            fprintf(stderr, "validate: import requires :subject\n");
            return false;
        }
        return true;
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

    pb_arena scratch = {0};
    pb_eval_state state = {0};
    pb_eval_ctx ctx = {
        .arena = &scratch,
        .state = &state,
        .rule_id = *rule_count - 1,
        .subject = {.ptr = "x.y.z", .len = 5},
        .payload = {.ptr = "1", .len = 1},
        .publish = discard_publish,
    };
    const pb_eval_result r = pb_eval(&ctx, items.items[body_idx]);
    pb_eval_state_free(&state);
    pb_arena_free(&scratch);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "validate: rule %zu eval failed: %s\n", *rule_count - 1, pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

int pb_validate_file(const char *path) {
    mb_buf source = {0};
    if (!mb_read_file(path, &source)) {
        perror(path);
        return 1;
    }

    pb_arena arena = {0};
    const pb_parse_result parsed = pb_parse_patchbay_source(&arena, path, (const char *)source.ptr, source.len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "validate: parse error: %s at byte %zu\n", pb_parse_error_name(parsed.err), parsed.err_offset);
        pb_arena_free(&arena);
        mb_buf_free(&source);
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
    mb_buf_free(&source);
    return ok ? 0 : 1;
}
