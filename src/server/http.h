#ifndef MB_HTTP_H
#define MB_HTTP_H

#include "server.h"

bool mb_http_listen(mb_server *server, const char *host, unsigned int port);
void mb_http_close(mb_server *server);

#endif
