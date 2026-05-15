#include "server.h"

#include "conn.h"
#include "snapshot.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>

struct mb_snapshot_job {
    uv_work_t req;
    mb_server *server;
    mb_buf snapshot;
    char *path;
    bool ok;
};

static void on_snapshot_timer(uv_timer_t *timer);

int64_t mb_wall_clock_ms(void) {
    uv_timeval64_t tv = {0};
    if (uv_gettimeofday(&tv) != 0) {
        return 0;
    }
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void on_patchbay_timer(uv_timer_t *timer) {
    mb_server *server = timer->data;
    uv_update_time(&server->loop);
    (void)pb_program_tick(server->program, &server->router, uv_now(&server->loop), mb_wall_clock_ms());
    mb_server_reschedule_patchbay_clock(server);
}

static void snapshot_work_cb(uv_work_t *req) {
    mb_snapshot_job *job = req->data;
    job->ok = mb_snapshot_write_bytes(job->path, &job->snapshot);
}

static void snapshot_after_work_cb(uv_work_t *req, int status) {
    mb_snapshot_job *job = req->data;
    mb_server *server = job->server;
    if (status == UV_ECANCELED) {
        fprintf(stderr, "warn: snapshot: write canceled: %s\n", job->path);
    } else if (job->ok) {
        fprintf(stderr, "info: snapshot: wrote %zu lvc / rule-state to %s\n",
                mb_router_lvc_count(&server->router), job->path);
    } else {
        fprintf(stderr, "warn: snapshot: write failed: %s\n", job->path);
    }

    mb_buf_free(&job->snapshot);
    free(job->path);
    free(job);
    server->snapshot_job = NULL;
    server->snapshot_write_pending = false;
    if (!server->closing && server->snapshot_write_again) {
        server->snapshot_write_again = false;
        on_snapshot_timer(&server->snapshot_timer);
    }
}

static void on_snapshot_timer(uv_timer_t *timer) {
    mb_server *server = timer->data;
    if (server->closing || server->snapshot_path == NULL) {
        return;
    }
    if (server->snapshot_write_pending) {
        server->snapshot_write_again = true;
        return;
    }

    mb_snapshot_job *job = calloc(1, sizeof *job);
    if (job == NULL) {
        fprintf(stderr, "warn: snapshot: allocate job failed: %s\n", server->snapshot_path);
        return;
    }
    const size_t path_len = strlen(server->snapshot_path);
    job->path = malloc(path_len + 1);
    if (job->path == NULL) {
        free(job);
        fprintf(stderr, "warn: snapshot: allocate path failed: %s\n", server->snapshot_path);
        return;
    }
    memcpy(job->path, server->snapshot_path, path_len + 1);
    if (!mb_snapshot_build(&job->snapshot, &server->router, server->program)) {
        fprintf(stderr, "warn: snapshot: build failed: %s\n", server->snapshot_path);
        free(job->path);
        free(job);
        return;
    }

    job->req.data = job;
    job->server = server;
    server->snapshot_write_pending = true;
    server->snapshot_job = job;
    const int rc = uv_queue_work(&server->loop, &job->req, snapshot_work_cb, snapshot_after_work_cb);
    if (rc != 0) {
        server->snapshot_write_pending = false;
        server->snapshot_job = NULL;
        mb_buf_free(&job->snapshot);
        free(job->path);
        free(job);
        fprintf(stderr, "warn: snapshot: queue failed: %s\n", uv_strerror(rc));
    }
}

static void on_signal(uv_signal_t *handle, int signum) {
    mb_server *server = handle->data;
    fprintf(stderr, "info: received %s, shutting down\n", signum == SIGINT ? "SIGINT" : "SIGTERM");
    server->closing = true;
    server->snapshot_write_again = false;
    if (server->sigint_started) {
        uv_signal_stop(&server->sigint);
    }
    if (server->sigterm_started) {
        uv_signal_stop(&server->sigterm);
    }
    uv_stop(&server->loop);
}

void mb_server_reschedule_patchbay_clock(mb_server *server) {
    if (!server->patchbay_timer_started) {
        return;
    }
    uint64_t deadline = 0;
    if (!pb_program_next_clock_deadline(server->program, &deadline)) {
        uv_timer_stop(&server->patchbay_timer);
        return;
    }
    uv_update_time(&server->loop);
    const uint64_t now = uv_now(&server->loop);
    const uint64_t due = deadline <= now ? 0 : deadline - now;
    (void)uv_timer_start(&server->patchbay_timer, on_patchbay_timer, due, 0);
}

static void on_connection(uv_stream_t *listener, int status) {
    mb_server *server = listener->data;
    if (status < 0) {
        fprintf(stderr, "accept error: %s\n", uv_strerror(status));
        return;
    }
    if (server->closing) {
        return;
    }
    if (!mb_conn_create(server, listener)) {
        fprintf(stderr, "failed to create connection\n");
    }
}

static void make_server_id(char out[35]) {
    uint8_t bytes[16] = {0};
    if (uv_random(NULL, NULL, bytes, sizeof bytes, 0, NULL) != 0) {
        uintptr_t seed = (uintptr_t)out;
        for (size_t i = 0; i < sizeof bytes; i += 1) {
            seed = seed * 6364136223846793005ULL + 1;
            bytes[i] = (uint8_t)(seed >> 24);
        }
    }
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'M';
    out[1] = 'C';
    for (size_t i = 0; i < sizeof bytes; i += 1) {
        out[2 + i * 2] = hex[bytes[i] >> 4];
        out[3 + i * 2] = hex[bytes[i] & 0xf];
    }
    out[34] = '\0';
}

bool mb_server_init(mb_server *server, const char *host, unsigned int port, pb_program *program,
                    bool lvc_enabled, const char *snapshot_path, uint64_t snapshot_every_ms, bool trace) {
    *server = (mb_server){
        .host = host,
        .port = port,
        .program = program,
        .lvc_enabled = lvc_enabled,
        .snapshot_path = snapshot_path,
        .snapshot_every_ms = snapshot_every_ms,
        .trace = trace,
    };
    make_server_id(server->server_id);
    server->next_client_id = 1;
    mb_router_init(&server->router);
    if (lvc_enabled && program != NULL && program->lvc.len != 0) {
        mb_slice *filters = calloc(program->lvc.len, sizeof filters[0]);
        if (filters == NULL) {
            return false;
        }
        for (size_t i = 0; i < program->lvc.len; i += 1) {
            filters[i] = (mb_slice){.ptr = (const uint8_t *)program->lvc.filters[i].ptr,
                                    .len = program->lvc.filters[i].len};
        }
        const bool ok = mb_router_configure_lvc(&server->router, filters, program->lvc.len);
        free(filters);
        if (!ok) {
            return false;
        }
    } else {
        mb_router_disable_lvc(&server->router);
    }
    if (snapshot_path != NULL) {
        mb_snapshot_counts counts = {0};
        if (!mb_snapshot_load(snapshot_path, &server->router, program, &counts)) {
            mb_router_free(&server->router);
            return false;
        }
        fprintf(stderr, "info: snapshot: loaded %zu lvc / %zu rule-state entries from %s\n",
                counts.lvc, counts.rule_state, snapshot_path);
    }
    if (uv_loop_init(&server->loop) != 0) {
        return false;
    }
    if (uv_tcp_init(&server->loop, &server->listener) != 0) {
        return false;
    }
    server->listener.data = server;
    if (uv_signal_init(&server->loop, &server->sigint) != 0) {
        return false;
    }
    server->sigint.data = server;
    if (uv_signal_start(&server->sigint, on_signal, SIGINT) != 0) {
        return false;
    }
    server->sigint_started = true;
    if (uv_signal_init(&server->loop, &server->sigterm) != 0) {
        return false;
    }
    server->sigterm.data = server;
    if (uv_signal_start(&server->sigterm, on_signal, SIGTERM) != 0) {
        return false;
    }
    server->sigterm_started = true;
    if (program != NULL && program->uses_clock_timer) {
        if (uv_timer_init(&server->loop, &server->patchbay_timer) != 0) {
            return false;
        }
        server->patchbay_timer.data = server;
        server->patchbay_timer_started = true;
    }
    if (snapshot_path != NULL && snapshot_every_ms != 0) {
        if (uv_timer_init(&server->loop, &server->snapshot_timer) != 0) {
            return false;
        }
        server->snapshot_timer.data = server;
        server->snapshot_timer_started = true;
        (void)uv_timer_start(&server->snapshot_timer, on_snapshot_timer, snapshot_every_ms, snapshot_every_ms);
    }

    struct sockaddr_in addr;
    const int ip_rc = uv_ip4_addr(host, (int)port, &addr);
    if (ip_rc != 0) {
        fprintf(stderr, "invalid address %s:%u: %s\n", host, port, uv_strerror(ip_rc));
        return false;
    }
    const int bind_rc = uv_tcp_bind(&server->listener, (const struct sockaddr *)&addr, 0);
    if (bind_rc != 0) {
        fprintf(stderr, "bind failed: %s\n", uv_strerror(bind_rc));
        return false;
    }
    const int listen_rc = uv_listen((uv_stream_t *)&server->listener, 128, on_connection);
    if (listen_rc != 0) {
        fprintf(stderr, "listen failed: %s\n", uv_strerror(listen_rc));
        return false;
    }
    return true;
}

int mb_server_run(mb_server *server) {
    printf("monoblok listening on %s:%u\n", server->host, server->port);
    fflush(stdout);
    return uv_run(&server->loop, UV_RUN_DEFAULT);
}

void mb_server_close(mb_server *server) {
    server->closing = true;
    server->snapshot_write_again = false;
    while (server->conns != NULL) {
        mb_conn_begin_close(server->conns);
    }
    if (!uv_is_closing((uv_handle_t *)&server->listener)) {
        uv_close((uv_handle_t *)&server->listener, NULL);
    }
    if (server->patchbay_timer_started && !uv_is_closing((uv_handle_t *)&server->patchbay_timer)) {
        uv_close((uv_handle_t *)&server->patchbay_timer, NULL);
    }
    if (server->snapshot_timer_started && !uv_is_closing((uv_handle_t *)&server->snapshot_timer)) {
        uv_close((uv_handle_t *)&server->snapshot_timer, NULL);
    }
    if (server->sigint_started && !uv_is_closing((uv_handle_t *)&server->sigint)) {
        uv_close((uv_handle_t *)&server->sigint, NULL);
    }
    if (server->sigterm_started && !uv_is_closing((uv_handle_t *)&server->sigterm)) {
        uv_close((uv_handle_t *)&server->sigterm, NULL);
    }
    if (server->snapshot_job != NULL) {
        (void)uv_cancel((uv_req_t *)&server->snapshot_job->req);
    }
    uv_run(&server->loop, UV_RUN_DEFAULT);
    while (server->snapshot_job != NULL) {
        if (uv_run(&server->loop, UV_RUN_DEFAULT) == 0) {
            break;
        }
    }
    if (server->snapshot_path != NULL) {
        if (mb_snapshot_write(server->snapshot_path, &server->router, server->program)) {
            fprintf(stderr, "info: shutdown: snapshot written (%zu lvc) to %s\n",
                    mb_router_lvc_count(&server->router), server->snapshot_path);
        } else {
            fprintf(stderr, "warn: shutdown: snapshot write failed: %s\n", server->snapshot_path);
        }
    }
    uv_loop_close(&server->loop);
    mb_router_free(&server->router);
}
