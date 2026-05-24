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
typedef struct mb_http_conn mb_http_conn;
typedef struct mb_snapshot_job mb_snapshot_job;
typedef void (*mb_server_stats_refresh_fn)(void *ctx);

typedef enum mb_auth_mode {
    MB_AUTH_NONE,
    MB_AUTH_TOKEN,
    MB_AUTH_USER_PASS,
} mb_auth_mode;

// Client auth settings copied from startup config; secret values borrow getenv storage.
typedef struct mb_auth_config {
    mb_auth_mode mode;
    const char *token;
    const char *user;
    const char *pass;
} mb_auth_config;

typedef enum mb_client_publish_status {
    MB_CLIENT_PUBLISH_OK,
    MB_CLIENT_PUBLISH_DISABLED,
    MB_CLIENT_PUBLISH_LVC_READ_ONLY,
    MB_CLIENT_PUBLISH_STATS_READ_ONLY,
    MB_CLIENT_PUBLISH_ROUTER_FAILED,
    MB_CLIENT_PUBLISH_PATCHBAY_FAILED,
} mb_client_publish_status;

// Process-local server state owned by one uv loop thread.
typedef struct mb_server {
    uv_loop_t loop;
    uv_tcp_t listener;
    uv_tcp_t http_listener;
    uv_signal_t sigint;
    uv_signal_t sigterm;
    uv_timer_t patchbay_timer;
    uv_timer_t snapshot_timer;
    uv_timer_t stats_timer;
    mb_router router;
    mb_auth_config auth;
    pb_program *program;
    mb_conn *conns;
    mb_http_conn *http_conns;
    mb_snapshot_job *snapshot_job;
    size_t conn_count;
    char server_id[35];
    uint64_t next_client_id;
    const char *host;
    const char *http_host;
    unsigned int port;
    unsigned int http_port;
    SSL_CTX *tls_ctx;
    const char *snapshot_path;
    uint64_t snapshot_every_ms;
    uint64_t stats_tick_ms;
    uint64_t total_pubs;
    int64_t patchbay_clock_offset_ms;
    const uint64_t *bridge_published;
    const uint64_t *bridge_dropped;
    const uint64_t *import_received;
    const uint64_t *import_processed;
    const uint64_t *import_dropped;
    const uint64_t *import_failed;
    mb_server_stats_refresh_fn stats_refresh;
    void *stats_refresh_ctx;
    bool patchbay_timer_started;
    bool snapshot_timer_started;
    bool stats_timer_started;
    bool listener_started;
    bool http_listener_initialized;
    bool http_listener_started;
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
                    const mb_auth_config *auth,
                    const char *tls_cert_path, const char *tls_key_path, bool listen_immediately);
bool mb_server_listen(mb_server *server);
int mb_server_run(mb_server *server);
void mb_server_close(mb_server *server);
bool mb_server_emit_stats(mb_server *server);
bool mb_auth_token_matches(const mb_auth_config *auth, mb_slice token);
bool mb_auth_user_pass_matches(const mb_auth_config *auth, mb_slice user, mb_slice pass);
mb_client_publish_status mb_server_client_publish(mb_server *server, mb_slice subject, mb_slice payload, mb_slice reply_to);
int64_t mb_wall_clock_ms(void);
uint64_t mb_server_patchbay_now_ms(mb_server *server);
void mb_server_set_patchbay_clock_offset(mb_server *server, int64_t offset_ms);
void mb_server_reschedule_patchbay_clock(mb_server *server);

#endif
