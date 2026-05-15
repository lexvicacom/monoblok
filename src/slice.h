#ifndef MB_SLICE_H
#define MB_SLICE_H

#include "proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool mb_slice_eq(mb_slice a, mb_slice b);
bool mb_slice_has_prefix(mb_slice s, const char *prefix, size_t prefix_len);
bool mb_slice_dup(mb_slice s, uint8_t **out_ptr, size_t *out_len);
uint64_t mb_slice_hash(mb_slice s);

#endif
