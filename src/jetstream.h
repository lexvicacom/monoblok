#ifndef MB_JETSTREAM_H
#define MB_JETSTREAM_H

#include "pb_program.h"
#include "router.h"

#include <stdbool.h>
#include <stdint.h>
#include <uv.h>

typedef struct mb_js_stream mb_js_stream;

typedef bool (*mb_js_import_handler)(void *ctx, const pb_import_stream_config *config,
                                     mb_slice subject, mb_slice payload,
                                     uint64_t event_now_ms, int64_t event_wall_ms,
                                     bool replaying);

// JetStream pull ingress; startup catch-up is serial, live delivery is backpressured.
typedef struct mb_js_importer {
    mb_js_stream *streams;
    size_t streams_len;
    const pb_imports_config *config;
    mb_js_import_handler handler;
    void *handler_ctx;
    uv_loop_t *loop;
    uint64_t received;
    uint64_t processed;
    uint64_t acked;
    uint64_t redelivered;
    uint64_t failed;
} mb_js_importer;

bool mb_js_importer_start(mb_js_importer *importer, uv_loop_t *loop, const pb_imports_config *config,
                          mb_js_import_handler handler, void *handler_ctx);
bool mb_js_importer_start_live(mb_js_importer *importer);
void mb_js_importer_refresh(mb_js_importer *importer);
void mb_js_importer_close(mb_js_importer *importer);

#endif
