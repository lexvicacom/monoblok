#include "server.h"

#include "conn.h"

#include <stdio.h>
#include <stdint.h>

static void on_connection(uv_stream_t *listener, int status) {
    mb_server *server = listener->data;
    if (status < 0) {
        fprintf(stderr, "accept error: %s\n", uv_strerror(status));
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

bool mb_server_init(mb_server *server, const char *host, unsigned int port, pb_program *program) {
    *server = (mb_server){.host = host, .port = port, .program = program};
    make_server_id(server->server_id);
    server->next_client_id = 1;
    mb_router_init(&server->router);
    if (uv_loop_init(&server->loop) != 0) {
        return false;
    }
    if (uv_tcp_init(&server->loop, &server->listener) != 0) {
        return false;
    }
    server->listener.data = server;

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
    printf("monoblok-c listening on %s:%u\n", server->host, server->port);
    return uv_run(&server->loop, UV_RUN_DEFAULT);
}

void mb_server_close(mb_server *server) {
    while (server->conns != NULL) {
        mb_conn_begin_close(server->conns);
    }
    if (!uv_is_closing((uv_handle_t *)&server->listener)) {
        uv_close((uv_handle_t *)&server->listener, NULL);
    }
    uv_run(&server->loop, UV_RUN_DEFAULT);
    uv_loop_close(&server->loop);
    mb_router_free(&server->router);
}
