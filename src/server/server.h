#ifndef MB_SERVER_H
#define MB_SERVER_H

#include "router.h"
#include "pb_program.h"

#include <stddef.h>
#include <stdint.h>
#include <uv.h>

typedef struct ssl_ctx_st SSL_CTX;

enum {
    // Accepted-client cap to bound slow or idle socket footprint.
    MB_MAX_CONNECTIONS = 1024,
    // Wall-clock cadence for cumulative `$STATS.*` publishes.
    MB_DEFAULT_STATS_TICK_MS = 60000,
};

typedef struct mb_conn mb_conn;
typedef struct mb_snapshot_job mb_snapshot_job;

// Process-local server state owned by one uv loop thread.
typedef struct mb_server {
    uv_loop_t loop;
    uv_tcp_t listener;
    uv_signal_t sigint;
    uv_signal_t sigterm;
    uv_timer_t patchbay_timer;
    uv_timer_t snapshot_timer;
    uv_timer_t stats_timer;
    mb_router router;
    pb_program *program;
    mb_conn *conns;
    mb_snapshot_job *snapshot_job;
    size_t conn_count;
    char server_id[35];
    uint64_t next_client_id;
    const char *host;
    unsigned int port;
    SSL_CTX *tls_ctx;
    const char *snapshot_path;
    uint64_t snapshot_every_ms;
    uint64_t stats_tick_ms;
    uint64_t total_pubs;
    const uint64_t *bridge_published;
    const uint64_t *bridge_dropped;
    const uint64_t *import_received;
    const uint64_t *import_processed;
    const uint64_t *import_dropped;
    const uint64_t *import_failed;
    bool patchbay_timer_started;
    bool snapshot_timer_started;
    bool stats_timer_started;
    bool sigint_started;
    bool sigterm_started;
    bool snapshot_write_pending;
    bool snapshot_write_again;
    bool closing;
    bool lvc_enabled;
    bool client_pubs_enabled;
    bool trace;
    bool tls_enabled;
} mb_server;

bool mb_server_init(mb_server *server, const char *host, unsigned int port, pb_program *program,
                    bool lvc_enabled, const char *snapshot_path, uint64_t snapshot_every_ms,
                    uint64_t stats_tick_ms, bool client_pubs_enabled, bool trace,
                    const char *tls_cert_path, const char *tls_key_path);
int mb_server_run(mb_server *server);
void mb_server_close(mb_server *server);
bool mb_server_emit_stats(mb_server *server);
int64_t mb_wall_clock_ms(void);
void mb_server_reschedule_patchbay_clock(mb_server *server);

#endif
