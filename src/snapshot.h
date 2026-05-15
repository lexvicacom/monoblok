#ifndef MB_SNAPSHOT_H
#define MB_SNAPSHOT_H

#include "router.h"
#include "pb_program.h"

// Counts of snapshot records loaded, useful for startup reporting.
typedef struct mb_snapshot_counts {
    size_t lvc;
    size_t rule_state;
} mb_snapshot_counts;

bool mb_snapshot_load(const char *path, mb_router *router, pb_program *program, mb_snapshot_counts *counts);
bool mb_snapshot_build(mb_buf *out, const mb_router *router, const pb_program *program);
bool mb_snapshot_write_bytes(const char *path, const mb_buf *snapshot);
bool mb_snapshot_write(const char *path, const mb_router *router, const pb_program *program);

#endif
