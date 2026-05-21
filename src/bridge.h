#ifndef MB_BRIDGE_H
#define MB_BRIDGE_H

#include "pb_program.h"

#include <stdint.h>

// Export-only NATS bridge; nats.c owns reconnects and outbound buffering.
typedef struct mb_bridge {
    void *conn;
    const pb_bridge_config *config;
    char *subject_scratch;
    char *origin_header_value;
    size_t subject_scratch_cap;
    uint64_t published;
    uint64_t dropped;
    int last_status;
    bool started;
} mb_bridge;

bool mb_bridge_start(mb_bridge *bridge, const pb_bridge_config *config);
void mb_bridge_publish(void *ctx, mb_slice subject, mb_slice payload);
void mb_bridge_publish_with_options(void *ctx, mb_slice subject, mb_slice payload, mb_router_publish_options options);
void mb_bridge_close(mb_bridge *bridge);

#endif
