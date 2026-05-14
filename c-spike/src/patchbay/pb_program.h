#ifndef PB_PROGRAM_H
#define PB_PROGRAM_H

#include "pb_eval.h"
#include "router.h"

typedef struct pb_rule {
    pb_slice filter;
    pb_value body;
    bool reentrant;
    pb_eval_state state;
} pb_rule;

// Loaded patchbay rules plus persistent rule state and reusable eval scratch.
typedef struct pb_program {
    pb_arena parse_arena;
    pb_arena scratch;
    pb_rule *rules;
    size_t len;
    size_t cap;
    bool uses_wall_clock;
    bool uses_clock_timer;
    size_t eval_depth;
} pb_program;

bool pb_program_load_file(pb_program *program, const char *path);
void pb_program_free(pb_program *program);
bool pb_program_eval_publish(pb_program *program, mb_router *router, mb_slice subject, mb_slice payload,
                             uint64_t now_ms, int64_t wall_ms);
bool pb_program_tick(pb_program *program, mb_router *router, uint64_t now_ms, int64_t wall_ms);
bool pb_program_next_clock_deadline(const pb_program *program, uint64_t *out_ms);

#endif
