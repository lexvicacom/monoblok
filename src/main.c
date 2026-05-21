#define _POSIX_C_SOURCE 200809L

#include "bridge.h"
#include "importer.h"
#include "jetstream.h"
#include "server.h"
#include "pb_soundcheck.h"
#include "pb_validate.h"
#include "pb_program.h"
#include "mb_version.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    printf("Usage: %s [PATCHBAY] [--host HOST] [--port PORT]\n"
           "\n"
           "Options:\n"
           "  PATCHBAY             Positional patchbay path for --validate/--soundcheck.\n"
           "  --patchbay FILE      Explicit patchbay path.\n"
           "  --host HOST          TCP listen host (default 127.0.0.1).\n"
           "  --port PORT          TCP listen port (default 4222).\n"
           "  --tls-cert FILE      Enable client TLS with this certificate chain PEM.\n"
           "  --tls-key FILE       Private key PEM for --tls-cert.\n"
           "  --no-lvc             Disable $LVC.* last-value cache streams.\n"
           "  --snapshot FILE      Load/write LVC and patchbay state snapshot.\n"
           "  --snapshot-every S   Periodically write snapshot every S seconds.\n"
           "  --stats-tick-ms MS   `$STATS.*` publish cadence (default 60000).\n"
           "  --trace              Log parsed client ops.\n"
           "  --io-uring           Enable libuv io_uring paths on Linux.\n"
           "  --no-io-uring        Disable libuv io_uring paths on Linux (default).\n"
           "  --validate           Parse and subset-lint the patchbay, then exit.\n"
           "  --soundcheck         Read SUBJECT|payload rows from stdin and print emits.\n"
           "  --soundcheck-label   Prefix soundcheck rows with in| or out|.\n"
           "  --soundcheck-linger-ms MS\n"
           "                       Keep clock/window state alive after soundcheck EOF (default 10000; 0 disables).\n"
           "  --help, -h           Show this help.\n"
           "  --version, -V        Show version.\n",
           argv0);
}

static void version(void) {
    printf("monoblok %s (%s)\n", MB_VERSION, MB_BUILD_INFO);
}

static void banner(void) {
    printf("\n"
           " __  __  ___  _  _  ___  ___  _    ___  _  __\n"
           "|  \\/  |/ _ \\| \\| |/ _ \\| _ )| |  / _ \\| |/ /\n"
           "| |\\/| | (_) | .` | (_) | _ \\| |_| (_) | ' <\n"
           "|_|  |_|\\___/|_|\\_|\\___/|___/|____\\___/|_|\\_\\\n"
           "\n"
           "monoblok v%s\n"
           "\n\n",
           MB_VERSION);
    fflush(stdout);
}

static const char *backend_name(void) {
#if defined(__APPLE__)
    return "kqueue";
#elif defined(__linux__)
    return "epoll";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return "kqueue";
#else
    return "unknown";
#endif
}

static const char *os_name(void) {
#if defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

static bool import_ingress(void *ctx, mb_slice subject, mb_slice payload) {
    mb_server *server = ctx;
    if (server == NULL || server->closing) {
        return false;
    }
    server->total_pubs += 1;
    uv_update_time(&server->loop);
    const bool ok = pb_program_eval_publish(server->program, &server->router, subject, payload,
                                            mb_server_patchbay_now_ms(server), mb_wall_clock_ms());
    mb_server_reschedule_patchbay_clock(server);
    return ok;
}

static bool jetstream_ingress(void *ctx, const pb_import_stream_config *config,
                              mb_slice subject, mb_slice payload,
                              uint64_t event_now_ms, int64_t event_wall_ms,
                              bool replaying) {
    (void)config;
    mb_server *server = ctx;
    if (server == NULL || server->closing) {
        return false;
    }
    pb_program_eval_options options = {.replaying = replaying};
    server->total_pubs += 1;
    const bool tick_ok = pb_program_tick_until(server->program, &server->router, event_now_ms, event_wall_ms, options);
    const bool eval_ok = pb_program_eval_publish_with_options(server->program, &server->router, subject, payload,
                                                              event_now_ms, event_wall_ms, options);
    if (!replaying) {
        mb_server_reschedule_patchbay_clock(server);
    }
    return tick_ok && eval_ok;
}

typedef struct import_group {
    mb_importer *items;
    size_t len;
    uint64_t received;
    uint64_t processed;
    uint64_t dropped;
    uint64_t failed;
} import_group;

typedef struct import_totals {
    import_group *core;
    mb_js_importer *jetstream;
    uint64_t received;
    uint64_t processed;
    uint64_t dropped;
    uint64_t failed;
} import_totals;

static void import_group_refresh(void *ctx) {
    import_group *group = ctx;
    if (group == NULL) {
        return;
    }
    group->received = 0;
    group->processed = 0;
    group->dropped = 0;
    group->failed = 0;
    for (size_t i = 0; i < group->len; i += 1) {
        group->received += group->items[i].received;
        group->processed += group->items[i].processed;
        group->dropped += group->items[i].dropped;
        group->failed += group->items[i].failed;
    }
}

static void import_group_close(import_group *group) {
    if (group == NULL) {
        return;
    }
    for (size_t i = 0; i < group->len; i += 1) {
        mb_importer_close(&group->items[i]);
    }
    free(group->items);
    *group = (import_group){0};
}

static bool import_group_start(import_group *group, uv_loop_t *loop, const pb_imports_config *config,
                               mb_importer_handler handler, void *handler_ctx) {
    if (config == NULL || config->cores_len == 0) {
        return true;
    }
    group->items = calloc(config->cores_len, sizeof group->items[0]);
    if (group->items == NULL) {
        return false;
    }
    group->len = config->cores_len;
    for (size_t i = 0; i < config->cores_len; i += 1) {
        if (!mb_importer_start(&group->items[i], loop, &config->cores[i], handler, handler_ctx)) {
            import_group_close(group);
            return false;
        }
    }
    import_group_refresh(group);
    return true;
}

static void import_totals_refresh(void *ctx) {
    import_totals *totals = ctx;
    if (totals == NULL) {
        return;
    }
    totals->received = 0;
    totals->processed = 0;
    totals->dropped = 0;
    totals->failed = 0;
    if (totals->core != NULL) {
        import_group_refresh(totals->core);
        totals->received += totals->core->received;
        totals->processed += totals->core->processed;
        totals->dropped += totals->core->dropped;
        totals->failed += totals->core->failed;
    }
    if (totals->jetstream != NULL) {
        mb_js_importer_refresh(totals->jetstream);
        totals->received += totals->jetstream->received;
        totals->processed += totals->jetstream->processed;
        totals->failed += totals->jetstream->failed;
    }
}

typedef enum {
    IO_URING_DEFAULT,
    IO_URING_ENABLE,
    IO_URING_DISABLE,
} io_uring_mode;

static const char *configure_libuv_io_uring(io_uring_mode mode) {
#if defined(__linux__)
    if (mode == IO_URING_ENABLE) {
        if (setenv("UV_USE_IO_URING", "1", 1) == 0) {
            return "enabled by --io-uring";
        }
        return "enable requested";
    }
    if (mode == IO_URING_DISABLE) {
        if (setenv("UV_USE_IO_URING", "0", 1) == 0) {
            return "disabled by --no-io-uring";
        }
        return "disable requested";
    }

    const char *configured = getenv("UV_USE_IO_URING");
    if (configured == NULL) {
        if (setenv("UV_USE_IO_URING", "0", 0) == 0) {
            return "disabled (default; set UV_USE_IO_URING=1 to enable)";
        }
        return "default";
    }
    return strcmp(configured, "0") == 0 ? "disabled by UV_USE_IO_URING" : "enabled by UV_USE_IO_URING";
#else
    (void)mode;
    return NULL;
#endif
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    unsigned long port = 4222;
    const char *soundcheck_path = NULL;
    const char *patchbay_path = NULL;
    const char *snapshot_path = NULL;
    const char *tls_cert_path = NULL;
    const char *tls_key_path = NULL;
    uint64_t snapshot_every_ms = 0;
    uint64_t stats_tick_ms = MB_DEFAULT_STATS_TICK_MS;
    uint64_t soundcheck_linger_ms = PB_SOUNDCHECK_DEFAULT_LINGER_MS;
    bool soundcheck = false;
    bool soundcheck_label = false;
    bool validate = false;
    bool lvc_enabled = true;
    bool trace = false;
    io_uring_mode io_uring = IO_URING_DEFAULT;

    for (int i = 1; i < argc; i += 1) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            errno = 0;
            char *end = NULL;
            port = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || port > 65535) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--io-uring") == 0) {
            io_uring = IO_URING_ENABLE;
        } else if (strcmp(argv[i], "--no-io-uring") == 0) {
            io_uring = IO_URING_DISABLE;
        } else if (strcmp(argv[i], "--no-lvc") == 0) {
            lvc_enabled = false;
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            tls_cert_path = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            tls_key_path = argv[++i];
        } else if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else if (strcmp(argv[i], "--snapshot-every") == 0 && i + 1 < argc) {
            errno = 0;
            char *end = NULL;
            const unsigned long seconds = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0') {
                usage(argv[0]);
                return 2;
            }
            snapshot_every_ms = (uint64_t)seconds * 1000;
        } else if (strcmp(argv[i], "--stats-tick-ms") == 0 && i + 1 < argc) {
            errno = 0;
            char *end = NULL;
            const unsigned long ms = strtoul(argv[++i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || ms == 0) {
                usage(argv[0]);
                return 2;
            }
            stats_tick_ms = (uint64_t)ms;
        } else if (strcmp(argv[i], "--trace") == 0) {
            trace = true;
        } else if (strcmp(argv[i], "--soundcheck") == 0) {
            soundcheck = true;
        } else if (strcmp(argv[i], "--soundcheck-label") == 0) {
            soundcheck_label = true;
        } else if (strcmp(argv[i], "--soundcheck-linger-ms") == 0 && i + 1 < argc) {
            errno = 0;
            char *end = NULL;
            const char *value = argv[++i];
            const unsigned long long ms = strtoull(value, &end, 10);
            if (errno != 0 || value[0] == '-' || end == value || *end != '\0') {
                usage(argv[0]);
                return 2;
            }
            soundcheck_linger_ms = (uint64_t)ms;
        } else if (strcmp(argv[i], "--patchbay") == 0 && i + 1 < argc) {
            patchbay_path = argv[++i];
        } else if (strcmp(argv[i], "--validate") == 0) {
            validate = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            version();
            return 0;
        } else if (argv[i][0] != '-' && patchbay_path == NULL) {
            patchbay_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if ((tls_cert_path == NULL) != (tls_key_path == NULL)) {
        usage(argv[0]);
        return 2;
    }

    soundcheck_path = patchbay_path;

    if (validate) {
        if (patchbay_path == NULL) {
            usage(argv[0]);
            return 2;
        }
        return pb_validate_file(patchbay_path);
    }

    if (soundcheck) {
        if (soundcheck_path == NULL) {
            usage(argv[0]);
            return 2;
        }
        return pb_soundcheck_run(soundcheck_path, (pb_soundcheck_options){
                                                  .linger_ms = soundcheck_linger_ms,
                                                  .label = soundcheck_label});
    }

    const char *io_uring_status = configure_libuv_io_uring(io_uring);

    banner();

    pb_program program = {0};
    pb_program *program_ptr = NULL;
    if (patchbay_path != NULL) {
        if (!pb_program_load_file(&program, patchbay_path)) {
            return 1;
        }
        program_ptr = &program;
    } else {
        fprintf(stderr, "info: no patchbay forms loaded\n");
    }
    fprintf(stderr, "info: libuv backend: %s (os=%s)\n", backend_name(), os_name());
    if (io_uring_status != NULL) {
        fprintf(stderr, "info: libuv io_uring: %s\n", io_uring_status);
    }
    const bool lvc_runtime_enabled = lvc_enabled && program_ptr != NULL && program.lvc.len != 0;
    fprintf(stderr, "info: lvc: %s\n", lvc_runtime_enabled ? "enabled" : "disabled");
    if (!lvc_enabled && program.lvc.len != 0) {
        fprintf(stderr, "warn: --no-lvc set: ignoring %zu configured LVC filter(s)\n", program.lvc.len);
    } else if (lvc_runtime_enabled) {
        fprintf(stderr, "info: lvc: configured %zu filter(s)\n", program.lvc.len);
    }
    if (snapshot_path != NULL) {
        fprintf(stderr, "info: snapshot: enabled path=%s auto flush=%s", snapshot_path,
                snapshot_every_ms == 0 ? "disabled" : "enabled");
        if (snapshot_every_ms != 0) {
            fprintf(stderr, " every=%" PRIu64 "s", snapshot_every_ms / 1000);
        }
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "info: snapshot: disabled\n");
    }
    fprintf(stderr, "info: stats: enabled every=%" PRIu64 "ms\n", stats_tick_ms);
    if (tls_cert_path == NULL) {
        fprintf(stderr, "info: tls: disabled\n");
    }

    mb_bridge bridge = {0};
    import_group imports = {0};
    mb_js_importer js_importer = {0};
    import_totals import_stats = {0};

    mb_server server;
    const bool client_pubs_enabled = !program.importer.present;
    const bool listen_immediately = program.importer.streams_len == 0;
    if (!mb_server_init(&server, host, (unsigned int)port, program_ptr, lvc_runtime_enabled, snapshot_path,
                        snapshot_every_ms, stats_tick_ms, client_pubs_enabled, trace,
                        tls_cert_path, tls_key_path, listen_immediately)) {
        pb_program_free(&program);
        return 1;
    }
    fprintf(stderr, "info: client pubs: %s\n", client_pubs_enabled ? "enabled" : "disabled (import mode)");
    if (program.bridge.present) {
        if (!mb_bridge_start(&bridge, &program.bridge)) {
            mb_server_close(&server);
            pb_program_free(&program);
            return 1;
        }
        server.router.bridge_ctx = &bridge;
        server.router.bridge_fn = mb_bridge_publish;
        server.bridge_published = &bridge.published;
        server.bridge_dropped = &bridge.dropped;
        fprintf(stderr, "info: bridge: connected to NATS (%zu server%s, %zu export filter%s)\n", program.bridge.servers_len,
                program.bridge.servers_len == 1 ? "" : "s", program.bridge.exports_len,
                program.bridge.exports_len == 1 ? "" : "s");
    } else {
        fprintf(stderr, "info: bridge: disabled\n");
    }
    if (program.importer.streams_len != 0) {
        if (!mb_js_importer_start(&js_importer, &server.loop, &program.importer, jetstream_ingress, &server)) {
            mb_js_importer_close(&js_importer);
            mb_server_close(&server);
            mb_bridge_close(&bridge);
            pb_program_free(&program);
            return 1;
        }
        uv_update_time(&server.loop);
        const int64_t wall_now = mb_wall_clock_ms();
        const uint64_t loop_now = uv_now(&server.loop);
        int64_t offset = 0;
        if (wall_now >= 0 && (uint64_t)wall_now >= loop_now && (uint64_t)wall_now - loop_now <= (uint64_t)INT64_MAX) {
            offset = (int64_t)((uint64_t)wall_now - loop_now);
        }
        mb_server_set_patchbay_clock_offset(&server, offset);
        (void)pb_program_tick_until(server.program, &server.router, (uint64_t)wall_now, wall_now,
                                    (pb_program_eval_options){.replaying = true});
        if (!mb_server_listen(&server) || !mb_js_importer_start_live(&js_importer)) {
            mb_js_importer_close(&js_importer);
            mb_server_close(&server);
            mb_bridge_close(&bridge);
            pb_program_free(&program);
            return 1;
        }
        fprintf(stderr, "info: jetstream import: connected to NATS (%zu stream source%s, serial catch-up complete)\n",
                program.importer.streams_len, program.importer.streams_len == 1 ? "" : "s");
    }
    if (program.importer.cores_len != 0) {
        if (!import_group_start(&imports, &server.loop, &program.importer, import_ingress, &server)) {
            import_group_close(&imports);
            mb_js_importer_close(&js_importer);
            mb_server_close(&server);
            mb_bridge_close(&bridge);
            pb_program_free(&program);
            return 1;
        }
        size_t subject_filters = 0;
        for (size_t i = 0; i < program.importer.cores_len; i += 1) {
            subject_filters += program.importer.cores[i].subjects_len;
        }
        fprintf(stderr, "info: import: connected to NATS (%zu core source%s, %zu subject filter%s)\n",
                program.importer.cores_len, program.importer.cores_len == 1 ? "" : "s",
                subject_filters, subject_filters == 1 ? "" : "s");
    }
    if (program.importer.present) {
        import_stats.core = imports.len == 0 ? NULL : &imports;
        import_stats.jetstream = js_importer.streams_len == 0 ? NULL : &js_importer;
        import_totals_refresh(&import_stats);
        server.import_received = &import_stats.received;
        server.import_processed = &import_stats.processed;
        server.import_dropped = &import_stats.dropped;
        server.import_failed = &import_stats.failed;
        server.stats_refresh = import_totals_refresh;
        server.stats_refresh_ctx = &import_stats;
    } else {
        fprintf(stderr, "info: import: disabled\n");
    }

    // block
    const int rc = mb_server_run(&server);

    // bye
    import_group_close(&imports);
    mb_js_importer_close(&js_importer);
    mb_server_close(&server);
    mb_bridge_close(&bridge);
    pb_program_free(&program);

    return rc == 0 ? 0 : 1;
}
