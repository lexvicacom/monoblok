#ifndef MB_SERVER_H
#define MB_SERVER_H

#include "router.h"
#include "pb_program.h"

#include <stdint.h>
#include <uv.h>

typedef struct mb_conn mb_conn;

// Process-local server state owned by one uv loop thread.
typedef struct mb_server {
    uv_loop_t loop;
    uv_tcp_t listener;
    mb_router router;
    pb_program *program;
    mb_conn *conns;
    char server_id[35];
    uint64_t next_client_id;
    const char *host;
    unsigned int port;
} mb_server;

bool mb_server_init(mb_server *server, const char *host, unsigned int port, pb_program *program);
int mb_server_run(mb_server *server);
void mb_server_close(mb_server *server);

#endif
