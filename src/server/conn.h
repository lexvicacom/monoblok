#ifndef MB_CONN_H
#define MB_CONN_H

#include "router.h"
#include "server.h"

#include <uv.h>

enum {
    MB_READ_CHUNK = 16 * 1024,
    MB_MAX_RX = MB_MAX_CONTROL_LINE + MB_MAX_PAYLOAD + 64,
    MB_MAX_PENDING = 64 * 1024 * 1024,
};

// libuv connection state, including exactly one guarded write in flight.
struct mb_conn {
    uv_tcp_t tcp;
    uv_write_t write_req;
    mb_server *server;
    mb_router_conn router_conn;
    mb_buf rx;
    mb_buf in_flight;
    uint8_t read_buf[MB_READ_CHUNK];
    char client_ip[64];
    uint64_t client_id;
    bool write_pending;
    bool closing;
    mb_conn *prev;
    mb_conn *next;
};

bool mb_conn_create(mb_server *server, uv_stream_t *listener);
void mb_conn_kick_write(void *ctx);
void mb_conn_begin_close(mb_conn *conn);

#endif
