#define _POSIX_C_SOURCE 200809L

#include "pb_config.h"

#include "array.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool value_is_env_form(pb_value value) {
    return value.kind == PB_LIST &&
           value.seq.len == 2 &&
           value.seq.items[0].kind == PB_SYMBOL &&
           pb_slice_eq_lit(value.seq.items[0].text, "env");
}

// Strings are literal or (env "NAME"); an unset or empty env var is "must not be empty".
static bool load_config_string(const pb_config_ctx *ctx, pb_value value, const char *form, const char *name, pb_slice *out) {
    if (value.kind != PB_STRING) {
        if (!value_is_env_form(value) || value.seq.items[1].kind != PB_STRING || value.seq.items[1].text.len == 0) {
            fprintf(stderr, "%s: %s :%s expects string or (env \"NAME\")\n", ctx->prefix, form, name);
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
        char *owned = pb_arena_memdup(ctx->arena, env_value, env_len);
        if (owned == NULL) {
            return false;
        }
        value = (pb_value){.kind = PB_STRING, .text = {.ptr = owned, .len = env_len}};
    }
    if (value.text.len == 0) {
        fprintf(stderr, "%s: %s :%s must not be empty\n", ctx->prefix, form, name);
        return false;
    }
    *out = value.text;
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

static bool load_config_bool(const pb_config_ctx *ctx, pb_value value, const char *form, const char *name, bool *out) {
    if (value.kind != PB_BOOL) {
        fprintf(stderr, "%s: %s :%s expects boolean\n", ctx->prefix, form, name);
        return false;
    }
    *out = value.boolean;
    return true;
}

static bool load_config_i64(const pb_config_ctx *ctx, pb_value value, const char *form, const char *name, int64_t *out) {
    if (value.kind != PB_NUMBER) {
        fprintf(stderr, "%s: %s :%s expects number\n", ctx->prefix, form, name);
        return false;
    }
    const int64_t n = (int64_t)value.number;
    if ((double)n != value.number) {
        fprintf(stderr, "%s: %s :%s expects integer number\n", ctx->prefix, form, name);
        return false;
    }
    *out = n;
    return true;
}

static bool load_config_int(const pb_config_ctx *ctx, pb_value value, const char *form, const char *name, int *out) {
    int64_t n = 0;
    if (!load_config_i64(ctx, value, form, name, &n)) {
        return false;
    }
    if (n < INT_MIN || n > INT_MAX) {
        fprintf(stderr, "%s: %s :%s is outside int range\n", ctx->prefix, form, name);
        return false;
    }
    *out = (int)n;
    return true;
}

static bool load_config_size(const pb_config_ctx *ctx, pb_value value, const char *form, const char *name, size_t *out) {
    int64_t n = 0;
    if (!load_config_i64(ctx, value, form, name, &n)) {
        return false;
    }
    if (n <= 0 || (uint64_t)n > SIZE_MAX) {
        fprintf(stderr, "%s: %s :%s must be a positive integer\n", ctx->prefix, form, name);
        return false;
    }
    *out = (size_t)n;
    return true;
}

static bool load_config_list(const pb_config_ctx *ctx, pb_slice **out, size_t *len, size_t *cap, pb_value value, const char *form, const char *name) {
    if (value.kind == PB_STRING || value_is_env_form(value)) {
        pb_slice single = {0};
        if (!load_config_string(ctx, value, form, name, &single)) {
            return false;
        }
        return slice_vec_append(out, len, cap, single);
    }
    if (value.kind != PB_VECTOR && value.kind != PB_LIST) {
        fprintf(stderr, "%s: %s :%s expects string, (env \"NAME\"), or vector/list of strings\n", ctx->prefix, form, name);
        return false;
    }
    if (value.seq.len == 0) {
        fprintf(stderr, "%s: %s :%s must not be empty\n", ctx->prefix, form, name);
        return false;
    }
    for (size_t i = 0; i < value.seq.len; i += 1) {
        pb_slice item = {0};
        if (!load_config_string(ctx, value.seq.items[i], form, name, &item)) {
            return false;
        }
        if (!slice_vec_append(out, len, cap, item)) {
            return false;
        }
    }
    return true;
}

// The one place the connection keyword ladder lives. Sets *matched = false for keys
// that belong to the calling form. `form` names the form in error messages.
static bool load_remote_option(const pb_config_ctx *ctx, pb_remote_config *remote, const char *form,
                               pb_slice key, pb_value value, bool *matched) {
    *matched = true;
    if (pb_slice_eq_lit(key, "servers")) {
        return load_config_list(ctx, &remote->servers, &remote->servers_len, &remote->servers_cap, value, form, "servers");
    }
    if (pb_slice_eq_lit(key, "name")) {
        return load_config_string(ctx, value, form, "name", &remote->name);
    }
    if (pb_slice_eq_lit(key, "creds")) {
        return load_config_string(ctx, value, form, "creds", &remote->creds);
    }
    if (pb_slice_eq_lit(key, "user")) {
        return load_config_string(ctx, value, form, "user", &remote->user);
    }
    if (pb_slice_eq_lit(key, "password")) {
        return load_config_string(ctx, value, form, "password", &remote->password);
    }
    if (pb_slice_eq_lit(key, "token")) {
        return load_config_string(ctx, value, form, "token", &remote->token);
    }
    if (pb_slice_eq_lit(key, "tls")) {
        return load_config_bool(ctx, value, form, "tls", &remote->tls);
    }
    if (pb_slice_eq_lit(key, "tls-ca")) {
        return load_config_string(ctx, value, form, "tls-ca", &remote->tls_ca);
    }
    if (pb_slice_eq_lit(key, "tls-cert")) {
        return load_config_string(ctx, value, form, "tls-cert", &remote->tls_cert);
    }
    if (pb_slice_eq_lit(key, "tls-key")) {
        return load_config_string(ctx, value, form, "tls-key", &remote->tls_key);
    }
    if (pb_slice_eq_lit(key, "tls-skip-verify")) {
        return load_config_bool(ctx, value, form, "tls-skip-verify", &remote->tls_skip_verify);
    }
    if (pb_slice_eq_lit(key, "connect-timeout-ms")) {
        remote->has_connect_timeout_ms = true;
        return load_config_i64(ctx, value, form, "connect-timeout-ms", &remote->connect_timeout_ms);
    }
    if (pb_slice_eq_lit(key, "ping-interval-ms")) {
        remote->has_ping_interval_ms = true;
        return load_config_i64(ctx, value, form, "ping-interval-ms", &remote->ping_interval_ms);
    }
    if (pb_slice_eq_lit(key, "reconnect-wait-ms")) {
        remote->has_reconnect_wait_ms = true;
        return load_config_i64(ctx, value, form, "reconnect-wait-ms", &remote->reconnect_wait_ms);
    }
    if (pb_slice_eq_lit(key, "max-reconnect")) {
        remote->has_max_reconnect = true;
        return load_config_int(ctx, value, form, "max-reconnect", &remote->max_reconnect);
    }
    *matched = false;
    return true;
}

void pb_remote_config_free(pb_remote_config *remote) {
    free(remote->servers);
    *remote = (pb_remote_config){0};
}

// ---- lvc ----

static bool lvc_vec_append(pb_lvc_config *lvc, pb_slice filter) {
    if (!mb_array_reserve((void **)&lvc->filters, &lvc->cap, lvc->len + 1, sizeof lvc->filters[0], 4)) {
        return false;
    }
    lvc->filters[lvc->len] = filter;
    lvc->len += 1;
    return true;
}

static bool load_lvc_filter_value(const pb_config_ctx *ctx, pb_lvc_config *lvc, pb_value value) {
    pb_slice filter = {0};
    if (!load_config_string(ctx, value, "lvc", "filter", &filter)) {
        return false;
    }
    return lvc_vec_append(lvc, filter);
}

static bool load_lvc_form(const pb_config_ctx *ctx, pb_lvc_config *lvc, pb_values items) {
    if (items.len < 2) {
        fprintf(stderr, "%s: lvc expects at least one filter\n", ctx->prefix);
        return false;
    }
    if (items.len == 2 && items.items[1].kind == PB_VECTOR) {
        pb_values filters = items.items[1].seq;
        if (filters.len == 0) {
            fprintf(stderr, "%s: lvc vector must not be empty\n", ctx->prefix);
            return false;
        }
        for (size_t i = 0; i < filters.len; i += 1) {
            if (!load_lvc_filter_value(ctx, lvc, filters.items[i])) {
                return false;
            }
        }
        return true;
    }
    for (size_t i = 1; i < items.len; i += 1) {
        if (!load_lvc_filter_value(ctx, lvc, items.items[i])) {
            return false;
        }
    }
    return true;
}

// ---- export / bridge ----

static bool load_export_form(const pb_config_ctx *ctx, pb_bridge_config *out, pb_values items, const char *form_name) {
    if (out->present) {
        fprintf(stderr, "%s: duplicate export/bridge form\n", ctx->prefix);
        return false;
    }
    if (items.len < 3 || (items.len % 2) == 0) {
        fprintf(stderr, "%s: %s expects keyword/value options\n", ctx->prefix, form_name);
        return false;
    }

    pb_bridge_config bridge = {.present = true};
    for (size_t i = 1; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "%s: %s option must be keyword\n", ctx->prefix, form_name);
            goto fail;
        }
        pb_slice key = items.items[i].text;
        pb_value value = items.items[i + 1];
        bool matched = false;
        if (!load_remote_option(ctx, &bridge.remote, form_name, key, value, &matched)) {
            goto fail;
        }
        if (matched) {
            continue;
        }
        if (pb_slice_eq_lit(key, "export")) {
            if (!load_config_list(ctx, &bridge.exports, &bridge.exports_len, &bridge.exports_cap, value, form_name, "export")) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "origin-header")) {
            if (!load_config_bool(ctx, value, form_name, "origin-header", &bridge.origin_header)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "replay-header")) {
            if (!load_config_bool(ctx, value, form_name, "replay-header", &bridge.replay_header)) {
                goto fail;
            }
        } else {
            fprintf(stderr, "%s: unknown %s option: %.*s\n", ctx->prefix, form_name, (int)key.len, key.ptr);
            goto fail;
        }
    }

    if (bridge.remote.servers_len == 0) {
        fprintf(stderr, "%s: %s requires :servers\n", ctx->prefix, form_name);
        goto fail;
    }
    *out = bridge;
    return true;

fail:
    pb_remote_config_free(&bridge.remote);
    free(bridge.exports);
    return false;
}

// ---- import ----

static void import_core_free(pb_import_config *config) {
    pb_remote_config_free(&config->remote);
    free(config->subjects);
    *config = (pb_import_config){0};
}

static void import_stream_free(pb_import_stream_config *stream) {
    import_core_free(&stream->source);
    *stream = (pb_import_stream_config){0};
}

static void import_set_free(pb_imports_config *imports) {
    for (size_t i = 0; i < imports->cores_len; i += 1) {
        import_core_free(&imports->cores[i]);
    }
    for (size_t i = 0; i < imports->streams_len; i += 1) {
        import_stream_free(&imports->streams[i]);
    }
    free(imports->cores);
    free(imports->streams);
    *imports = (pb_imports_config){0};
}

static bool import_core_append(pb_imports_config *imports, pb_import_config config) {
    if (!mb_array_reserve((void **)&imports->cores, &imports->cores_cap, imports->cores_len + 1,
                          sizeof imports->cores[0], 2)) {
        return false;
    }
    imports->cores[imports->cores_len] = config;
    imports->cores_len += 1;
    return true;
}

static bool import_stream_append(pb_imports_config *imports, pb_import_stream_config stream) {
    if (!mb_array_reserve((void **)&imports->streams, &imports->streams_cap, imports->streams_len + 1,
                          sizeof imports->streams[0], 2)) {
        return false;
    }
    imports->streams[imports->streams_len] = stream;
    imports->streams_len += 1;
    return true;
}

static bool import_key_is_nested(pb_slice key) {
    return pb_slice_eq_lit(key, "core") || pb_slice_eq_lit(key, "streams");
}

// Options common to core and stream entries: the remote ladder plus subject(s),
// origin-header, and max-pending. Returns false on a bad value; *matched is false
// when the key belongs to neither.
static bool load_import_common_option(const pb_config_ctx *ctx, pb_import_config *importer,
                                      pb_slice key, pb_value value, bool *matched) {
    if (!load_remote_option(ctx, &importer->remote, "import", key, value, matched)) {
        return false;
    }
    if (*matched) {
        return true;
    }
    *matched = true;
    if (pb_slice_eq_lit(key, "subject") || pb_slice_eq_lit(key, "subjects")) {
        return load_config_list(ctx, &importer->subjects, &importer->subjects_len, &importer->subjects_cap, value, "import", "subject");
    }
    if (pb_slice_eq_lit(key, "origin-header")) {
        return load_config_bool(ctx, value, "import", "origin-header", &importer->origin_header);
    }
    if (pb_slice_eq_lit(key, "max-pending")) {
        return load_config_size(ctx, value, "import", "max-pending", &importer->max_pending);
    }
    *matched = false;
    return true;
}

static bool validate_import_core(const pb_config_ctx *ctx, const pb_import_config *importer, const char *label) {
    if (importer->remote.servers_len == 0) {
        fprintf(stderr, "%s: %s requires :servers\n", ctx->prefix, label);
        return false;
    }
    if (importer->subjects_len == 0) {
        fprintf(stderr, "%s: %s requires :subject\n", ctx->prefix, label);
        return false;
    }
    return true;
}

static bool load_import_core_entry(const pb_config_ctx *ctx, pb_values items, const char *label, pb_import_config *out) {
    if (items.len == 0 || (items.len % 2) != 0) {
        fprintf(stderr, "%s: %s expects keyword/value options\n", ctx->prefix, label);
        return false;
    }
    pb_import_config importer = {.present = true};
    for (size_t i = 0; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "%s: %s option must be keyword\n", ctx->prefix, label);
            goto fail;
        }
        bool matched = false;
        if (!load_import_common_option(ctx, &importer, items.items[i].text, items.items[i + 1], &matched)) {
            goto fail;
        }
        if (!matched) {
            fprintf(stderr, "%s: unknown %s option: %.*s\n", ctx->prefix, label, (int)items.items[i].text.len, items.items[i].text.ptr);
            goto fail;
        }
    }
    if (!validate_import_core(ctx, &importer, label)) {
        goto fail;
    }
    *out = importer;
    return true;

fail:
    import_core_free(&importer);
    return false;
}

static bool load_import_stream_entry(const pb_config_ctx *ctx, pb_values items, const char *label, pb_import_stream_config *out) {
    if (items.len == 0 || (items.len % 2) != 0) {
        fprintf(stderr, "%s: %s expects keyword/value options\n", ctx->prefix, label);
        return false;
    }
    pb_import_stream_config stream = {.source = {.present = true}, .catch_up = true};
    for (size_t i = 0; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "%s: %s option must be keyword\n", ctx->prefix, label);
            goto fail;
        }
        const pb_slice key = items.items[i].text;
        pb_value value = items.items[i + 1];
        bool matched = false;
        if (!load_import_common_option(ctx, &stream.source, key, value, &matched)) {
            goto fail;
        }
        if (matched) {
            continue;
        }
        if (pb_slice_eq_lit(key, "stream")) {
            if (!load_config_string(ctx, value, "import", "stream", &stream.stream)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "consumer")) {
            if (!load_config_string(ctx, value, "import", "consumer", &stream.consumer)) {
                goto fail;
            }
        } else if (pb_slice_eq_lit(key, "catch-up")) {
            if (!load_config_bool(ctx, value, "import", "catch-up", &stream.catch_up)) {
                goto fail;
            }
        } else {
            fprintf(stderr, "%s: unknown %s option: %.*s\n", ctx->prefix, label, (int)key.len, key.ptr);
            goto fail;
        }
    }
    if (!validate_import_core(ctx, &stream.source, label)) {
        goto fail;
    }
    if (stream.source.subjects_len != 1) {
        fprintf(stderr, "%s: %s expects exactly one :subject filter in v1\n", ctx->prefix, label);
        goto fail;
    }
    if (stream.stream.len == 0) {
        fprintf(stderr, "%s: %s requires :stream\n", ctx->prefix, label);
        goto fail;
    }
    if (stream.consumer.len == 0) {
        fprintf(stderr, "%s: %s requires :consumer\n", ctx->prefix, label);
        goto fail;
    }
    *out = stream;
    return true;

fail:
    import_stream_free(&stream);
    return false;
}

static bool import_entry_values(const pb_config_ctx *ctx, pb_value value, const char *label, pb_values *out) {
    if (value.kind != PB_VECTOR) {
        fprintf(stderr, "%s: %s entry must be a vector of keyword/value options\n", ctx->prefix, label);
        return false;
    }
    *out = value.seq;
    return true;
}

static bool load_import_core_entries(const pb_config_ctx *ctx, pb_imports_config *imports, pb_value value) {
    if (value.kind != PB_VECTOR || value.seq.len == 0) {
        fprintf(stderr, "%s: import :core expects a non-empty vector of entries\n", ctx->prefix);
        return false;
    }
    for (size_t i = 0; i < value.seq.len; i += 1) {
        pb_values entry = {0};
        pb_import_config core = {0};
        if (!import_entry_values(ctx, value.seq.items[i], "import :core", &entry) ||
            !load_import_core_entry(ctx, entry, "import :core", &core)) {
            return false;
        }
        if (!import_core_append(imports, core)) {
            import_core_free(&core);
            return false;
        }
    }
    return true;
}

static bool load_import_stream_entries(const pb_config_ctx *ctx, pb_imports_config *imports, pb_value value) {
    if (value.kind != PB_VECTOR || value.seq.len == 0) {
        fprintf(stderr, "%s: import :streams expects a non-empty vector of entries\n", ctx->prefix);
        return false;
    }
    for (size_t i = 0; i < value.seq.len; i += 1) {
        pb_values entry = {0};
        pb_import_stream_config stream = {0};
        if (!import_entry_values(ctx, value.seq.items[i], "import :streams", &entry) ||
            !load_import_stream_entry(ctx, entry, "import :streams", &stream)) {
            return false;
        }
        if (!import_stream_append(imports, stream)) {
            import_stream_free(&stream);
            return false;
        }
    }
    return true;
}

static bool load_import_form(const pb_config_ctx *ctx, pb_imports_config *out, pb_values items) {
    if (out->present) {
        fprintf(stderr, "%s: duplicate import form\n", ctx->prefix);
        return false;
    }
    if (items.len < 3 || (items.len % 2) == 0) {
        fprintf(stderr, "%s: import expects keyword/value options\n", ctx->prefix);
        return false;
    }

    bool saw_nested = false;
    bool saw_flat = false;
    for (size_t i = 1; i < items.len; i += 2) {
        if (items.items[i].kind != PB_KEYWORD) {
            fprintf(stderr, "%s: import option must be keyword\n", ctx->prefix);
            return false;
        }
        if (import_key_is_nested(items.items[i].text)) {
            saw_nested = true;
        } else {
            saw_flat = true;
        }
    }
    if (saw_nested && saw_flat) {
        fprintf(stderr, "%s: import cannot mix nested :core/:streams with flat options\n", ctx->prefix);
        return false;
    }

    pb_imports_config imports = {.present = true};
    if (!saw_nested) {
        pb_import_config core = {0};
        if (!load_import_core_entry(ctx, (pb_values){.items = items.items + 1, .len = items.len - 1}, "import", &core)) {
            goto fail;
        }
        if (!import_core_append(&imports, core)) {
            import_core_free(&core);
            goto fail;
        }
    } else {
        for (size_t i = 1; i < items.len; i += 2) {
            const pb_slice key = items.items[i].text;
            pb_value value = items.items[i + 1];
            if (pb_slice_eq_lit(key, "core")) {
                if (!load_import_core_entries(ctx, &imports, value)) {
                    goto fail;
                }
            } else if (pb_slice_eq_lit(key, "streams")) {
                if (!load_import_stream_entries(ctx, &imports, value)) {
                    goto fail;
                }
            } else {
                fprintf(stderr, "%s: unknown import option: %.*s\n", ctx->prefix, (int)key.len, key.ptr);
                goto fail;
            }
        }
    }

    if (imports.cores_len == 0 && imports.streams_len == 0) {
        fprintf(stderr, "%s: import requires at least one :core or :streams entry\n", ctx->prefix);
        goto fail;
    }
    *out = imports;
    return true;

fail:
    import_set_free(&imports);
    return false;
}

// ---- dispatch ----

bool pb_config_head_is(pb_slice head) {
    return pb_slice_eq_lit(head, "lvc") || pb_slice_eq_lit(head, "export") ||
           pb_slice_eq_lit(head, "bridge") || pb_slice_eq_lit(head, "import");
}

bool pb_config_load_form(const pb_config_ctx *ctx, pb_values items,
                         pb_lvc_config *lvc, pb_bridge_config *bridge, pb_imports_config *imports) {
    const pb_slice head = items.items[0].text;
    if (pb_slice_eq_lit(head, "lvc")) {
        return load_lvc_form(ctx, lvc, items);
    }
    if (pb_slice_eq_lit(head, "export")) {
        return load_export_form(ctx, bridge, items, "export");
    }
    if (pb_slice_eq_lit(head, "bridge")) {
        return load_export_form(ctx, bridge, items, "bridge");
    }
    if (pb_slice_eq_lit(head, "import")) {
        return load_import_form(ctx, imports, items);
    }
    return true;
}

void pb_config_free(pb_lvc_config *lvc, pb_bridge_config *bridge, pb_imports_config *imports) {
    free(lvc->filters);
    *lvc = (pb_lvc_config){0};
    pb_remote_config_free(&bridge->remote);
    free(bridge->exports);
    *bridge = (pb_bridge_config){0};
    import_set_free(imports);
}
