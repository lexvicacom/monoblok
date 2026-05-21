#ifndef MB_IMPORTER_H
#define MB_IMPORTER_H

#include "buf.h"
#include "pb_program.h"
#include "router.h"

#include <stdbool.h>
#include <stdint.h>
#include <uv.h>

enum {
    // Bounded imported-message backlog between nats.c callback threads and the uv loop.
    MB_IMPORT_DEFAULT_MAX_PENDING = 4096,
};

typedef bool (*mb_importer_handler)(void *ctx, mb_slice subject, mb_slice payload);

// Inbound NATS tap; callbacks copy into a bounded ring and loop-thread drain runs patchbay.
typedef struct mb_importer {
    void *conn;
    void **subs;
    size_t subs_len;
    size_t subs_cap;
    const pb_import_config *config;
    mb_importer_handler handler;
    void *handler_ctx;
    uv_loop_t *loop;
    uv_async_t async;
    uv_mutex_t lock;
    uv_thread_t thread;
    mb_buf drain_scratch;
    struct mb_import_slot *ring;
    size_t ring_cap;
    size_t head;
    size_t len;
    uint64_t pending_received;
    uint64_t pending_dropped;
    uint64_t received;
    uint64_t processed;
    uint64_t dropped;
    uint64_t failed;
    int last_status;
    bool lock_started;
    bool async_started;
    bool async_closed;
    bool thread_started;
    bool started;
    bool closing;
} mb_importer;

bool mb_importer_start(mb_importer *importer, uv_loop_t *loop, const pb_import_config *config,
                       mb_importer_handler handler, void *handler_ctx);
void mb_importer_drain(mb_importer *importer);
void mb_importer_close(mb_importer *importer);

#endif
