#include "array.h"

#include <stdint.h>
#include <stdlib.h>

bool mb_array_reserve(void **items, size_t *cap, size_t needed,
                      size_t elem_size, size_t initial_cap) {
    if (items == NULL || cap == NULL || elem_size == 0) {
        return false;
    }
    if (*cap >= needed) {
        return true;
    }

    size_t next = *cap == 0 ? initial_cap : *cap;
    if (next == 0) {
        next = 1;
    }

    while (next < needed) {
        if (next > SIZE_MAX / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }

    if (next > SIZE_MAX / elem_size) {
        return false;
    }

    void *new_items = realloc(*items, next * elem_size);
    if (new_items == NULL) {
        return false;
    }

    *items = new_items;
    *cap = next;
    return true;
}
