#include "slice.h"

#include <stdlib.h>
#include <string.h>

bool mb_slice_eq(mb_slice a, mb_slice b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool mb_slice_eq_bytes(const uint8_t *ptr, size_t len, mb_slice s) {
    return len == s.len && memcmp(ptr, s.ptr, len) == 0;
}

bool mb_slice_has_prefix(mb_slice s, const char *prefix, size_t prefix_len) {
    return s.len >= prefix_len && memcmp(s.ptr, prefix, prefix_len) == 0;
}

bool mb_slice_dup(mb_slice s, uint8_t **out_ptr, size_t *out_len) {
    if (out_ptr == NULL || out_len == NULL) {
        return false;
    }

    uint8_t *ptr = malloc(s.len == 0 ? 1 : s.len);
    if (ptr == NULL) {
        return false;
    }
    if (s.len != 0) {
        memcpy(ptr, s.ptr, s.len);
    }

    *out_ptr = ptr;
    *out_len = s.len;
    return true;
}

uint64_t mb_slice_hash(mb_slice s) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < s.len; i += 1) {
        h ^= (uint64_t)s.ptr[i];
        h *= 1099511628211ULL;
    }
    return h;
}
