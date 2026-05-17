#define _POSIX_C_SOURCE 200809L

#include "bridge.h"
#include "server.h"
#include "pb_soundcheck.h"
#include "pb_validate.h"
#include "pb_program.h"
#include "mb_version.h"

#include <errno.h>
#include <inttypes.h>
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
    uint64_t snapshot_every_ms = 0;
    uint64_t stats_tick_ms = MB_DEFAULT_STATS_TICK_MS;
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
        return pb_soundcheck_run(soundcheck_path, (pb_soundcheck_options){.label = soundcheck_label});
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

    mb_bridge bridge = {0};

    mb_server server;
    if (!mb_server_init(&server, host, (unsigned int)port, program_ptr, lvc_runtime_enabled, snapshot_path,
                        snapshot_every_ms, stats_tick_ms, trace)) {
        pb_program_free(&program);
        return 1;
    }
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

    // block
    const int rc = mb_server_run(&server);

    // bye
    mb_server_close(&server);
    mb_bridge_close(&bridge);
    pb_program_free(&program);

    return rc == 0 ? 0 : 1;
}
