#define _POSIX_C_SOURCE 200809L

#include "pb_program.h"

#include "array.h"
#include "fs.h"
#include "pb_eval_internal.h"
#include "pb_json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct publish_ctx {
    pb_program *program;
    mb_router *router;
    uint64_t now_ms;
    int64_t wall_ms;
    size_t rule_idx;
    size_t depth;
    bool reentrant;
} publish_ctx;

enum {
    PB_PROGRAM_MAX_REENTRY_DEPTH = 8,
    PB_PROGRAM_SCRATCH_RETAIN_BYTES = 4 * 1024 * 1024,
};

static void reset_scratch(pb_program *program) {
    pb_arena_reset(&program->scratch);
    pb_arena_trim(&program->scratch, PB_PROGRAM_SCRATCH_RETAIN_BYTES);
}

static bool slice_match(pb_slice filter, pb_slice subject) {
    return mb_router_subject_matches((mb_slice){.ptr = (const uint8_t *)filter.ptr, .len = filter.len},
                                     (mb_slice){.ptr = (const uint8_t *)subject.ptr, .len = subject.len});
}

static bool rule_vec_append(pb_program *program, pb_rule rule) {
    if (!mb_array_reserve((void **)&program->rules, &program->cap, program->len + 1,
                          sizeof program->rules[0], 8)) {
        return false;
    }
    program->rules[program->len] = rule;
    program->len += 1;
    return true;
}

static bool lvc_vec_append(pb_program *program, pb_slice filter) {
    if (!mb_array_reserve((void **)&program->lvc.filters, &program->lvc.cap,
                          program->lvc.len + 1, sizeof program->lvc.filters[0], 4)) {
        return false;
    }
    program->lvc.filters[program->lvc.len] = filter;
    program->lvc.len += 1;
    return true;
}

static bool value_is_env_form(pb_value value) {
    return value.kind == PB_LIST &&
           value.seq.len == 2 &&
           value.seq.items[0].kind == PB_SYMBOL &&
           pb_slice_eq_lit(value.seq.items[0].text, "env");
}

static bool load_config_string(pb_program *program, pb_value value, const char *form, const char *name, pb_slice *out) {
    if (value.kind != PB_STRING) {
        if (!value_is_env_form(value) || value.seq.items[1].kind != PB_STRING || value.seq.items[1].text.len == 0) {
            fprintf(stderr, "patchbay: %s :%s expects string or (env \"NAME\")\n", form, name);
            return false;
        }
        pb_slice env_name = value.seq.items[1].text;
        char *name_buf = malloc(env_name.len + 1);
        if (name_buf == NULL) {
            return false;
        }
        memcpy(name_buf, env_name.ptr, env_name.len);
        name_buf[env_name.len] = '\0';
        const char *env_value = getenv(name_buf);
        free(name_buf);
        if (env_value == NULL) {
            env_value = "";
        }
        const size_t env_len = strlen(env_value);
        char *owned = pb_arena_memdup(&program->parse_arena, env_value, env_len);
        if (owned == NULL) {
            return false;
        }
        value = (pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = env_len}};
    }
    if (value.text.len == 0) {
        fprintf(stderr, "patchbay: %s :%s must not be empty\n", form, name);
        return false;
    }
    *out = value.text;
    return true;
}

static bool load_lvc_filter_value(pb_program *program, pb_value value) {
    pb_slice filter = {0};
    if (!load_config_string(program, value, "lvc", "filter", &filter)) {
        return false;
    }
    return lvc_vec_append(program, filter);
}

static bool load_lvc_form(pb_program *program, pb_values items) {
    if (items.len < 2) {
        fprintf(stderr, "patchbay: lvc expects at least one filter\n");
        return false;
    }
    if (items.len == 2 && items.items[1].kind == PB_VECTOR) {
        pb_values filters = items.items[1].seq;
        if (filters.len == 0) {
            fprintf(stderr, "patchbay: lvc vector must not be empty\n");
            return false;
        }
        for (size_t i = 0; i < filters.len; i += 1) {
            if (!load_lvc_filter_value(program, filters.items[i])) {
                return false;
            }
        }
        return true;
    }
    for (size_t i = 1; i < items.len; i += 1) {
        if (!load_lvc_filter_value(program, items.items[i])) {
            return false;
        }
    }
    return true;
}

static bool slice_vec_append(pb_slice **items, size_t *len, size_t *cap, pb_slice value) {
    if (!mb_array_reserve((void **)items, cap, *len + 1, sizeof (*items)[0], 4)) {
        return false;
    }
    (*items)[*len] = value;
    *len += 1;
    return true;
}

static bool load_remote_bool(pb_value value, const char *form, const char *name, bool *out) {
    if (value.kind != PB_BOOL) {
        fprintf(stderr, "patchbay: %s :%s expects boolean\n", form, name);
        return false;
    }
    *out = value.boolean;
    return true;
}

static bool load_remote_i64(pb_value value, const char *form, const char *name, int64_t *out) {
    if (value.kind != PB_NUMBER) {
        fprintf(stderr, "patchbay: %s :%s expects number\n", form, name);
        return false;
    }
    const int64_t n = (int64_t)value.number;
    if ((double)n != value.number) {
        fprintf(stderr, "patchbay: %s :%s expects integer number\n", form, name);
        return false;
    }
    *out = n;
    return true;
}

static bool load_remote_int(pb_value value, const char *form, const char *name, int *out) {
    int64_t n = 0;
    if (!load_remote_i64(value, form, name, &n)) {
        return false;
    }
    if (n < INT_MIN || n > INT_MAX) {
        fprintf(stderr, "patchbay: %s :%s is outside int range\n", form, name);
        return false;
    }
    *out = (int)n;
    return true;
}

static bool load_remote_size(pb_value value, const char *form, const char *name, size_t *out) {
    int64_t n = 0;
    if (!load_remote_i64(value, form, name, &n)) {
        return false;
    }
    if (n <= 0 || (uint64_t)n > SIZE_MAX) {
        fprintf(stderr, "patchbay: %s :%s must be a positive integer\n", form, name);
        return false;
    }
    *out = (size_t)n;
    return true;
}

static bool load_remote_list(pb_program *program, pb_slice **out, size_t *len, size_t *cap, pb_value value, const char *form, const char *name) {
    if (value.kind == PB_STRING || value_is_env_form(value)) {
        pb_slice single = {0};
        if (!load_config_string(program, value, form, name, &single)) {
            return false;
        }
        return slice_vec_append(out, len, cap, single);
    }
    if (value.kind != PB_VECTOR && value.kind != PB_LIST) {
        fprintf(stderr, "patchbay: %s :%s expects string, (env \"NAME\"), or vector/list of strings\n", form, name);
        return false;
    }
    if (value.seq.len == 0) {
        fprintf(stderr, "patchbay: %s :%s must not be empty\n", form, name);
        return false;
    }
    for (size_t i = 0; i < value.seq.len; i += 1) {
        pb_slice item = {0};
        if (!load_config_string(program, value.seq.items[i], form, name, &item)) {
            return false;
        }
        if (!slice_vec_append(out, len, cap, item)) {
            return false;
        }
    }
    return true;
}

static bool is_export_config_head(pb_slice head) {
    return pb_slice_eq_lit(head, "bridge") || pb_slice_eq_lit(head, "export");
}

static const char *export_config_name(pb_slice head) {
    return pb_slice_eq_lit(head, "export") ? "export" : "bridge";
}

static bool load_bridge_bool(pb_value value, const char *form_name, const char *name, bool *out) {
    return load_remote_bool(value, form_name, name, out);
}

static bool load_bridge_i64(pb_value value, const char *form_name, const char *name, int64_t *out) {
    return load_remote_i64(value, form_name, name, out);
}

static bool load_bridge_int(pb_value value, const char *form_name, const char *name, int *out) {
    return load_remote_int(value, form_name, name, out);
}

static bool load_bridge_form(pb_program *program, pb_values items, const char *form_name) {
    if (program->bridge.present) {
        fprintf(stderr, "patchbay: duplicate export/bridge form\n");
        return false;
    }
    if (items.len < 3 || (items.len % 2) == 0) {
        fprintf(stderr, "patchbay: %s expects keyword/value options\n", form_name);
        return false;
    }

    pb_bridge_config bridge = {0};
    bridge.present = true;
    for (size_t i = 1; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "patchbay: %s option must be keyword\n", form_name);
            goto fail;
        }
        pb_slice key = items.items[i].text;
        pb_value value = items.items[i + 1];
        if (pb_slice_eq_lit(key, "servers")) {
            if (!load_remote_list(program, &bridge.servers, &bridge.servers_len, &bridge.servers_cap, value, form_name, "servers")) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "export")) {
            if (!load_remote_list(program, &bridge.exports, &bridge.exports_len, &bridge.exports_cap, value, form_name, "export")) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "name")) {
            if (!load_config_string(program, value, form_name, "name", &bridge.name)) {
                goto fail;
            }
            bridge.has_name = true;
        } else if (pb_slice_eq_lit(key, "creds")) {
            if (!load_config_string(program, value, form_name, "creds", &bridge.creds)) {
                goto fail;
            }
            bridge.has_creds = true;
        } else if (pb_slice_eq_lit(key, "user")) {
            if (!load_config_string(program, value, form_name, "user", &bridge.user)) {
                goto fail;
            }
            bridge.has_user = true;
        } else if (pb_slice_eq_lit(key, "password")) {
            if (!load_config_string(program, value, form_name, "password", &bridge.password)) {
                goto fail;
            }
            bridge.has_password = true;
        } else if (pb_slice_eq_lit(key, "token")) {
            if (!load_config_string(program, value, form_name, "token", &bridge.token)) {
                goto fail;
            }
            bridge.has_token = true;
        } else if (pb_slice_eq_lit(key, "tls")) {
            if (!load_bridge_bool(value, form_name, "tls", &bridge.tls)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "tls-ca")) {
            if (!load_config_string(program, value, form_name, "tls-ca", &bridge.tls_ca)) {
                goto fail;
            }
            bridge.has_tls_ca = true;
        } else if (pb_slice_eq_lit(key, "tls-cert")) {
            if (!load_config_string(program, value, form_name, "tls-cert", &bridge.tls_cert)) {
                goto fail;
            }
            bridge.has_tls_cert = true;
        } else if (pb_slice_eq_lit(key, "tls-key")) {
            if (!load_config_string(program, value, form_name, "tls-key", &bridge.tls_key)) {
                goto fail;
            }
            bridge.has_tls_key = true;
        } else if (pb_slice_eq_lit(key, "tls-skip-verify")) {
            if (!load_bridge_bool(value, form_name, "tls-skip-verify", &bridge.tls_skip_verify)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "origin-header")) {
            if (!load_bridge_bool(value, form_name, "origin-header", &bridge.origin_header)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "connect-timeout-ms")) {
            if (!load_bridge_i64(value, form_name, "connect-timeout-ms", &bridge.connect_timeout_ms)) {
                goto fail;
            }
            bridge.has_connect_timeout_ms = true;
        } else if (pb_slice_eq_lit(key, "ping-interval-ms")) {
            if (!load_bridge_i64(value, form_name, "ping-interval-ms", &bridge.ping_interval_ms)) {
                goto fail;
            }
            bridge.has_ping_interval_ms = true;
        } else if (pb_slice_eq_lit(key, "reconnect-wait-ms")) {
            if (!load_bridge_i64(value, form_name, "reconnect-wait-ms", &bridge.reconnect_wait_ms)) {
                goto fail;
            }
            bridge.has_reconnect_wait_ms = true;
        } else if (pb_slice_eq_lit(key, "max-reconnect")) {
            if (!load_bridge_int(value, form_name, "max-reconnect", &bridge.max_reconnect)) {
                goto fail;
            }
            bridge.has_max_reconnect = true;
        } else {
            fprintf(stderr, "patchbay: unknown %s option: %.*s\n", form_name, (int)key.len, key.ptr);
            goto fail;
        }
    }

    if (bridge.servers_len == 0) {
        fprintf(stderr, "patchbay: %s requires :servers\n", form_name);
        goto fail;
    }
    program->bridge = bridge;
    return true;

fail:
    free(bridge.servers);
    free(bridge.exports);
    return false;
}

static bool load_import_bool(pb_value value, const char *name, bool *out) {
    return load_remote_bool(value, "import", name, out);
}

static bool load_import_i64(pb_value value, const char *name, int64_t *out) {
    return load_remote_i64(value, "import", name, out);
}

static bool load_import_int(pb_value value, const char *name, int *out) {
    return load_remote_int(value, "import", name, out);
}

static bool load_import_size(pb_value value, const char *name, size_t *out) {
    return load_remote_size(value, "import", name, out);
}

static bool load_import_form(pb_program *program, pb_values items) {
    if (program->importer.present) {
        fprintf(stderr, "patchbay: duplicate import form\n");
        return false;
    }
    if (items.len < 3 || (items.len % 2) == 0) {
        fprintf(stderr, "patchbay: import expects keyword/value options\n");
        return false;
    }

    pb_import_config importer = {0};
    importer.present = true;
    for (size_t i = 1; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "patchbay: import option must be keyword\n");
            goto fail;
        }
        pb_slice key = items.items[i].text;
        pb_value value = items.items[i + 1];
        if (pb_slice_eq_lit(key, "servers")) {
            if (!load_remote_list(program, &importer.servers, &importer.servers_len, &importer.servers_cap, value, "import", "servers")) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "subject") || pb_slice_eq_lit(key, "subjects")) {
            if (!load_remote_list(program, &importer.subjects, &importer.subjects_len, &importer.subjects_cap, value, "import", "subject")) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "name")) {
            if (!load_config_string(program, value, "import", "name", &importer.name)) {
                goto fail;
            }
            importer.has_name = true;
        } else if (pb_slice_eq_lit(key, "creds")) {
            if (!load_config_string(program, value, "import", "creds", &importer.creds)) {
                goto fail;
            }
            importer.has_creds = true;
        } else if (pb_slice_eq_lit(key, "user")) {
            if (!load_config_string(program, value, "import", "user", &importer.user)) {
                goto fail;
            }
            importer.has_user = true;
        } else if (pb_slice_eq_lit(key, "password")) {
            if (!load_config_string(program, value, "import", "password", &importer.password)) {
                goto fail;
            }
            importer.has_password = true;
        } else if (pb_slice_eq_lit(key, "token")) {
            if (!load_config_string(program, value, "import", "token", &importer.token)) {
                goto fail;
            }
            importer.has_token = true;
        } else if (pb_slice_eq_lit(key, "tls")) {
            if (!load_import_bool(value, "tls", &importer.tls)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "tls-ca")) {
            if (!load_config_string(program, value, "import", "tls-ca", &importer.tls_ca)) {
                goto fail;
            }
            importer.has_tls_ca = true;
        } else if (pb_slice_eq_lit(key, "tls-cert")) {
            if (!load_config_string(program, value, "import", "tls-cert", &importer.tls_cert)) {
                goto fail;
            }
            importer.has_tls_cert = true;
        } else if (pb_slice_eq_lit(key, "tls-key")) {
            if (!load_config_string(program, value, "import", "tls-key", &importer.tls_key)) {
                goto fail;
            }
            importer.has_tls_key = true;
        } else if (pb_slice_eq_lit(key, "tls-skip-verify")) {
            if (!load_import_bool(value, "tls-skip-verify", &importer.tls_skip_verify)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "origin-header")) {
            if (!load_import_bool(value, "origin-header", &importer.origin_header)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "connect-timeout-ms")) {
            if (!load_import_i64(value, "connect-timeout-ms", &importer.connect_timeout_ms)) {
                goto fail;
            }
            importer.has_connect_timeout_ms = true;
        } else if (pb_slice_eq_lit(key, "ping-interval-ms")) {
            if (!load_import_i64(value, "ping-interval-ms", &importer.ping_interval_ms)) {
                goto fail;
            }
            importer.has_ping_interval_ms = true;
        } else if (pb_slice_eq_lit(key, "reconnect-wait-ms")) {
            if (!load_import_i64(value, "reconnect-wait-ms", &importer.reconnect_wait_ms)) {
                goto fail;
            }
            importer.has_reconnect_wait_ms = true;
        } else if (pb_slice_eq_lit(key, "max-reconnect")) {
            if (!load_import_int(value, "max-reconnect", &importer.max_reconnect)) {
                goto fail;
            }
            importer.has_max_reconnect = true;
        } else if (pb_slice_eq_lit(key, "max-pending")) {
            if (!load_import_size(value, "max-pending", &importer.max_pending)) {
                goto fail;
            }
            importer.has_max_pending = true;
        } else {
            fprintf(stderr, "patchbay: unknown import option: %.*s\n", (int)key.len, key.ptr);
            goto fail;
        }
    }

    if (importer.servers_len == 0) {
        fprintf(stderr, "patchbay: import requires :servers\n");
        goto fail;
    }
    if (importer.subjects_len == 0) {
        fprintf(stderr, "patchbay: import requires :subject\n");
        goto fail;
    }
    program->importer = importer;
    return true;

fail:
    free(importer.servers);
    free(importer.subjects);
    return false;
}

static bool load_on_form(pb_program *program, pb_value form) {
    if (form.kind != PB_LIST || form.seq.len == 0 || form.seq.items[0].kind != PB_SYMBOL) {
        fprintf(stderr, "patchbay: top-level form must be a list headed by a symbol\n");
        return false;
    }
    pb_values items = form.seq;
    if (pb_slice_eq_lit(items.items[0].text, "lvc")) {
        return load_lvc_form(program, items);
    }
    if (is_export_config_head(items.items[0].text)) {
        return load_bridge_form(program, items, export_config_name(items.items[0].text));
    }
    if (pb_slice_eq_lit(items.items[0].text, "import")) {
        return load_import_form(program, items);
    }
    if (!pb_slice_eq_lit(items.items[0].text, "on")) {
        return true;
    }
    if (items.len < 3 || items.items[1].kind != PB_STRING) {
        fprintf(stderr, "patchbay: invalid on form\n");
        return false;
    }

    pb_rule rule = {
        .filter = items.items[1].text,
        .body = items.items[items.len - 1],
    };
    const size_t body_idx = items.len - 1;
    size_t i = 2;
    while (i < body_idx) {
        if (i + 1 >= body_idx || items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "patchbay: invalid on options\n");
            return false;
        }
        if (!pb_slice_eq_lit(items.items[i].text, "reentrant")) {
            fprintf(stderr, "patchbay: unknown on option: %.*s\n", (int)items.items[i].text.len,
                    items.items[i].text.ptr);
            return false;
        }
        if (items.items[i + 1].kind != PB_BOOL) {
            fprintf(stderr, "patchbay: :reentrant expects boolean\n");
            return false;
        }
        rule.reentrant = items.items[i + 1].boolean;
        i += 2;
    }
    return rule_vec_append(program, rule);
}

static bool first_subject_token(pb_slice s, pb_slice *tok) {
    size_t end = 0;
    while (end < s.len && s.ptr[end] != '.') {
        end += 1;
    }
    if (end == 0) {
        return false;
    }
    *tok = (pb_slice){.ptr = s.ptr, .len = end};
    return true;
}

static bool wildcard_token(pb_slice tok) {
    return tok.len == 1 && (tok.ptr[0] == '*' || tok.ptr[0] == '>');
}

static bool rule_ref_append(pb_rule_ref_list *list, size_t rule_idx) {
    if (!mb_array_reserve((void **)&list->items, &list->cap, list->len + 1,
                          sizeof list->items[0], 8)) {
        return false;
    }
    list->items[list->len] = rule_idx;
    list->len += 1;
    return true;
}

static pb_rule_bucket *rule_bucket(pb_program *program, pb_slice key) {
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        if (pb_slice_eq(program->rule_buckets[i].key, key)) {
            return &program->rule_buckets[i];
        }
    }
    if (!mb_array_reserve((void **)&program->rule_buckets, &program->rule_bucket_cap,
                          program->rule_bucket_len + 1, sizeof program->rule_buckets[0], 8)) {
        return NULL;
    }
    pb_rule_bucket *bucket = &program->rule_buckets[program->rule_bucket_len];
    *bucket = (pb_rule_bucket){.key = key};
    program->rule_bucket_len += 1;
    return bucket;
}

static void rule_index_free(pb_program *program) {
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        free(program->rule_buckets[i].rules.items);
    }
    free(program->rule_buckets);
    free(program->rule_global.items);
    program->rule_buckets = NULL;
    program->rule_bucket_len = 0;
    program->rule_bucket_cap = 0;
    program->rule_global = (pb_rule_ref_list){0};
}

static bool build_rule_index(pb_program *program) {
    rule_index_free(program);
    for (size_t i = 0; i < program->len; i += 1) {
        pb_slice first = {0};
        if (!first_subject_token(program->rules[i].filter, &first) || wildcard_token(first)) {
            if (!rule_ref_append(&program->rule_global, i)) {
                return false;
            }
            continue;
        }
        pb_rule_bucket *bucket = rule_bucket(program, first);
        if (bucket == NULL || !rule_ref_append(&bucket->rules, i)) {
            return false;
        }
    }
    return true;
}

static bool value_head_eq(pb_value v, const char *lit) {
    return v.kind == PB_LIST && v.seq.len > 0 && v.seq.items[0].kind == PB_SYMBOL && pb_slice_eq_lit(v.seq.items[0].text, lit);
}

static bool call_has_ms_window(pb_value v) {
    return v.kind == PB_LIST && v.seq.len >= 3 && v.seq.items[1].kind == PB_KEYWORD &&
           pb_slice_eq_lit(v.seq.items[1].text, "ms");
}

static void scan_clock_forms(pb_program *program, pb_value v) {
    if (value_head_eq(v, "now")) {
        program->uses_wall_clock = true;
    }
    if ((value_head_eq(v, "bar!") || value_head_eq(v, "bar") ||
         value_head_eq(v, "moving-avg") || value_head_eq(v, "moving-sum") ||
         value_head_eq(v, "moving-min") || value_head_eq(v, "moving-max") ||
         value_head_eq(v, "median") || value_head_eq(v, "percentile") ||
         value_head_eq(v, "stddev") || value_head_eq(v, "variance") ||
         value_head_eq(v, "rate") || value_head_eq(v, "throttle")) &&
        call_has_ms_window(v)) {
        program->uses_clock_timer = true;
    }
    if (value_head_eq(v, "on-silence") ||
        value_head_eq(v, "dropout") || value_head_eq(v, "debounce!") ||
        value_head_eq(v, "sample!") || value_head_eq(v, "aggregate!")) {
        program->uses_clock_timer = true;
    }
    if (v.kind == PB_LIST || v.kind == PB_VECTOR) {
        for (size_t i = 0; i < v.seq.len; i += 1) {
            scan_clock_forms(program, v.seq.items[i]);
        }
    }
}

static size_t count_print_forms(pb_value v) {
    size_t n = value_head_eq(v, "print!") ? 1 : 0;
    if (v.kind == PB_LIST || v.kind == PB_VECTOR) {
        for (size_t i = 0; i < v.seq.len; i += 1) {
            n += count_print_forms(v.seq.items[i]);
        }
    }
    return n;
}

static size_t count_rule_print_forms(pb_value v) {
    if (!value_head_eq(v, "on") || v.seq.len < 3) {
        return 0;
    }
    return count_print_forms(v.seq.items[v.seq.len - 1]);
}

static bool load_source(pb_program *program, const char *label, const char *source, size_t source_len, bool log) {
    pb_eval_symbol_fn user_symbol = program->user_symbol;
    pb_eval_call_fn user_call = program->user_call;
    void *user_ctx = program->user_ctx;
    *program = (pb_program){
        .user_symbol = user_symbol,
        .user_call = user_call,
        .user_ctx = user_ctx,
    };
    const pb_parse_result parsed = pb_parse_patchbay_source(&program->parse_arena, label, source, source_len);
    if (parsed.err != PB_PARSE_OK) {
        fprintf(stderr, "patchbay: parse error in %s: %s at byte %zu\n", label, pb_parse_error_name(parsed.err),
                parsed.err_offset);
        pb_program_free(program);
        return false;
    }
    size_t print_calls = 0;
    size_t print_rules = 0;
    for (size_t i = 0; i < parsed.forms.len; i += 1) {
        if (!load_on_form(program, parsed.forms.items[i])) {
            pb_program_free(program);
            return false;
        }
        scan_clock_forms(program, parsed.forms.items[i]);
        const size_t form_prints = count_rule_print_forms(parsed.forms.items[i]);
        if (form_prints != 0) {
            print_calls += form_prints;
            print_rules += 1;
        }
    }
    if (!build_rule_index(program)) {
        pb_program_free(program);
        return false;
    }
    if (log) {
        fprintf(stderr, "info: loaded %zu patchbay form(s) from %s\n", program->len, label);
        if (program->uses_wall_clock) {
            fprintf(stderr, "info: patchbay wallclock: enabled\n");
        }
        if (program->uses_clock_timer) {
            fprintf(stderr, "info: patchbay clock: enabled (one-shot deadlines)\n");
        }
        if (print_calls != 0) {
            fprintf(stderr,
                    "warning: patchbay contains %zu print! call(s) across %zu rule(s); will log payload data to stderr and add per-call overhead (debug aid, do not leave in production)\n",
                    print_calls, print_rules);
        }
    }
    return true;
}

bool pb_program_load_source(pb_program *program, const char *label, const char *source, size_t source_len) {
    return load_source(program, label, source, source_len, false);
}

void pb_program_set_eval_hooks(pb_program *program, pb_eval_symbol_fn user_symbol, pb_eval_call_fn user_call, void *user_ctx) {
    if (program == NULL) {
        return;
    }
    program->user_symbol = user_symbol;
    program->user_call = user_call;
    program->user_ctx = user_ctx;
}

bool pb_program_load_file(pb_program *program, const char *path) {
    mb_buf source = {0};
    if (!mb_read_file(path, &source)) {
        perror(path);
        *program = (pb_program){0};
        return false;
    }

    const bool ok = load_source(program, path, (const char *)source.ptr, source.len, true);
    mb_buf_free(&source);
    return ok;
}

void pb_program_free(pb_program *program) {
    for (size_t i = 0; i < program->len; i += 1) {
        pb_eval_state_free(&program->rules[i].state);
    }
    rule_index_free(program);
    free(program->rules);
    free(program->lvc.filters);
    free(program->bridge.servers);
    free(program->bridge.exports);
    free(program->importer.servers);
    free(program->importer.subjects);
    pb_arena_free(&program->scratch);
    pb_arena_free(&program->parse_arena);
    *program = (pb_program){0};
}

static const pb_rule_ref_list *rule_refs_for_subject(const pb_program *program, pb_slice subject) {
    pb_slice first = {0};
    if (!first_subject_token(subject, &first)) {
        return NULL;
    }
    for (size_t i = 0; i < program->rule_bucket_len; i += 1) {
        const pb_rule_bucket *bucket = &program->rule_buckets[i];
        if (pb_slice_eq(bucket->key, first)) {
            return &bucket->rules;
        }
    }
    return NULL;
}

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                uint64_t now_ms, int64_t wall_ms, size_t depth);

static bool publish_cb(void *ctx, pb_slice subject, pb_slice payload) {
    publish_ctx *p = ctx;
    const mb_slice mb_subject = {.ptr = (const uint8_t *)subject.ptr, .len = subject.len};
    if (!mb_proto_subject_valid(mb_subject, false) ||
        mb_router_subject_has_lvc_prefix(mb_subject) ||
        mb_router_subject_has_stats_prefix(mb_subject) ||
        !mb_router_publish(p->router, mb_subject,
                           (mb_slice){.ptr = (const uint8_t *)payload.ptr, .len = payload.len})) {
        return false;
    }
    if (p->program != NULL && p->rule_idx < p->program->len) {
        p->program->rules[p->rule_idx].publishes_emitted += 1;
    }
    if (!p->reentrant) {
        return true;
    }
    if (p->depth >= PB_PROGRAM_MAX_REENTRY_DEPTH) {
        fprintf(stderr, "patchbay: reentry depth cap reached\n");
        return true;
    }
    (void)eval_publish_slices(p->program, p->router, subject, payload, p->now_ms, p->wall_ms, p->depth + 1);
    return true;
}

static bool eval_rule_for_publish(pb_program *program, mb_router *router, size_t rule_idx,
                                  pb_slice subject, pb_slice payload,
                                  uint64_t now_ms, int64_t wall_ms, size_t depth) {
    pb_rule *rule = &program->rules[rule_idx];
    if (!slice_match(rule->filter, subject)) {
        return true;
    }
    publish_ctx pub = {
        .program = program,
        .router = router,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .rule_idx = rule_idx,
        .depth = depth,
        .reentrant = rule->reentrant,
    };
    pb_eval_ctx ctx = {
        .arena = &program->scratch,
        .state = &rule->state,
        .rule_id = rule_idx,
        .now_ms = now_ms,
        .wall_ms = wall_ms,
        .subject = subject,
        .payload = payload,
        .publish = publish_cb,
        .publish_ctx = &pub,
        .publishes_emitted = &rule->publishes_emitted,
        .publishes_suppressed = &rule->publishes_suppressed,
        .user_symbol = program->user_symbol,
        .user_call = program->user_call,
        .user_ctx = program->user_ctx,
    };
    const pb_eval_result r = pb_eval(&ctx, rule->body);
    if (r.err != PB_EVAL_OK) {
        fprintf(stderr, "patchbay: rule %zu eval failed: %s\n", rule_idx, pb_eval_error_name(r.err));
        return false;
    }
    return true;
}

static bool eval_publish_slices(pb_program *program, mb_router *router, pb_slice subject, pb_slice payload,
                                uint64_t now_ms, int64_t wall_ms, size_t depth) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    bool ok = true;
    const pb_rule_ref_list *bucket = rule_refs_for_subject(program, subject);
    size_t bucket_pos = 0;
    size_t global_pos = 0;
    while ((bucket != NULL && bucket_pos < bucket->len) || global_pos < program->rule_global.len) {
        const size_t bucket_idx = bucket != NULL && bucket_pos < bucket->len ? bucket->items[bucket_pos] : SIZE_MAX;
        const size_t global_idx = global_pos < program->rule_global.len ? program->rule_global.items[global_pos] : SIZE_MAX;
        size_t rule_idx = 0;
        if (bucket_idx < global_idx) {
            rule_idx = bucket_idx;
            bucket_pos += 1;
        } else {
            rule_idx = global_idx;
            global_pos += 1;
        }
        if (!eval_rule_for_publish(program, router, rule_idx, subject, payload, now_ms, wall_ms, depth)) {
            ok = false;
        }
    }
    return ok;
}

bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms) {
    if (program == NULL || program->len == 0) {
        return true;
    }
    const bool outer = program->eval_depth == 0;
    program->eval_depth += 1;
    const bool ok =
        eval_publish_slices(program, router, (pb_slice){.ptr = (const char *)subject.ptr, .len = subject.len},
                            (pb_slice){.ptr = (const char *)payload.ptr, .len = payload.len}, now_ms, wall_ms, 0);
    program->eval_depth -= 1;
    if (outer) {
        reset_scratch(program);
    }
    return ok;
}

bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms) {
    if (program == NULL || program->len == 0) {
        return true;
    }

    const bool outer = program->eval_depth == 0;
    program->eval_depth += 1;
    bool ok = true;
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1) {
        pb_rule *rule = &program->rules[rule_idx];
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1) {
            pb_eval_state_entry *entry = &rule->state.items[state_idx];
            publish_ctx pub = {
                .program = program,
                .router = router,
                .now_ms = now_ms,
                .wall_ms = wall_ms,
                .rule_idx = rule_idx,
                .depth = 0,
                .reentrant = rule->reentrant,
            };
            pb_eval_ctx ctx = {
                .arena = &program->scratch,
                .state = &rule->state,
                .rule_id = rule_idx,
                .now_ms = now_ms,
                .wall_ms = wall_ms,
                .subject = {.ptr = entry->subject, .len = entry->subject_len},
                .payload = {.ptr = "", .len = 0},
                .publish = publish_cb,
                .publish_ctx = &pub,
                .publishes_emitted = &rule->publishes_emitted,
                .publishes_suppressed = &rule->publishes_suppressed,
                .user_symbol = program->user_symbol,
                .user_call = program->user_call,
                .user_ctx = program->user_ctx,
            };
            const pb_eval_result r = pb_eval_tick_state_entry(&ctx, entry);
            if (r.err != PB_EVAL_OK) {
                fprintf(stderr, "patchbay: rule %zu clock tick failed: %s\n", rule_idx, pb_eval_error_name(r.err));
                ok = false;
            }
        }
    }
    program->eval_depth -= 1;
    if (outer) {
        reset_scratch(program);
    }
    return ok;
}

static bool entry_deadline(const pb_eval_state_entry *entry, uint64_t *out_ms) {
    if (entry->kind == PB_EVAL_STATE_RING && entry->ring_time_window && entry->ring_len > 0) {
        const size_t idx = entry->ring_start;
        *out_ms = entry->ring_times_ms[idx] + entry->ring_window_ms;
        return true;
    }
    if (entry->kind == PB_EVAL_STATE_BAR && entry->bar_time_window && entry->bar_count > 0) {
        *out_ms = entry->bar_window_start_ms + entry->bar_window_ms;
        return true;
    }
    if (entry->kind == PB_EVAL_STATE_CLOCK && entry->clock_armed) {
        *out_ms = entry->clock_deadline_ms;
        return true;
    }
    return false;
}

bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms) {
    bool found = false;
    uint64_t best = 0;
    if (program == NULL || !program->uses_clock_timer) {
        return false;
    }
    for (size_t rule_idx = 0; rule_idx < program->len; rule_idx += 1) {
        const pb_rule *rule = &program->rules[rule_idx];
        for (size_t state_idx = 0; state_idx < rule->state.len; state_idx += 1) {
            uint64_t deadline = 0;
            if (entry_deadline(&rule->state.items[state_idx], &deadline) && (!found || deadline < best)) {
                best = deadline;
                found = true;
            }
        }
    }
    if (found) {
        *out_ms = best;
    }
    return found;
}
