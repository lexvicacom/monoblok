#ifndef MB_ARRAY_H
#define MB_ARRAY_H

#include <stdbool.h>
#include <stddef.h>

// Reserve capacity for a simple growable C array.
bool mb_array_reserve(void **items, size_t *cap, size_t needed,
                      size_t elem_size, size_t initial_cap);

#endif
