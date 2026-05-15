#ifndef MB_ARRAY_H
#define MB_ARRAY_H

#include <stdbool.h>
#include <stddef.h>

// Reserve capacity for a simple growable C array.
//
// `items` points at the array pointer, `cap` is the current element capacity,
// and `needed` is the minimum element count required by the caller. The helper
// doubles capacity from `initial_cap`, checks all size arithmetic, and only
// updates `items`/`cap` after realloc succeeds.
bool mb_array_reserve(void **items, size_t *cap, size_t needed,
                      size_t elem_size, size_t initial_cap);

#endif
