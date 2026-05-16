#ifndef PB_JSON_H
#define PB_JSON_H

#include "pb_sexpr.h"

#ifndef PB_ENABLE_JSON
#define PB_ENABLE_JSON 1
#endif

pb_parse_result pb_parse_patchbay_source(pb_arena *arena, const char *path, const char *src, size_t len);

#endif
