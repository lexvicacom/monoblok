#ifndef MB_NATS_COMMON_H
#define MB_NATS_COMMON_H

#include "pb_program.h"

#include <stdbool.h>
#include <stddef.h>

#include "nats.h"

#define MB_NATS_ORIGIN_HEADER "x-monoblok"
#define MB_NATS_REPLAY_HEADER "x-monoblok-replay"
#define MB_NATS_ASSUMED_TS_HEADER "x-monoblok-assumed-ts"

// Temporary C strings passed into nats.c option setup.
typedef struct mb_nats_strings {
    char **items;
    size_t len;
    size_t cap;
} mb_nats_strings;

bool mb_nats_set_options(natsOptions *opts, const pb_remote_config *config, mb_nats_strings *strings);
char *mb_nats_track_cstr(mb_nats_strings *strings, pb_slice slice);
char *mb_nats_origin_header_value(void);
void mb_nats_strings_free(mb_nats_strings *strings);
void mb_nats_retain(void);
void mb_nats_release(void);

#endif
