#include "slice.h"

#include <stdlib.h>
#include <string.h>

bool mb_slice_eq(mb_slice a, mb_slice b) {
    return a.len == b.len && memcmp(a.ptr, b.ptr, a.len) == 0;
}

bool mb_slice_has_prefix(mb_slice s, const char *prefix, size_t prefix_len) {
    return s.len >= prefix_len && memcmp(s.ptr, prefix, prefix_len) == 0;
}

static uint8_t ascii_lower(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}

bool mb_slice_has_prefix_ci_lit(mb_slice s, const char *prefix) {
    const size_t n = strlen(prefix);
    if (s.len < n) {
        return false;
    }
    for (size_t i = 0; i < n; i += 1) {
        if (ascii_lower(s.ptr[i]) != ascii_lower((uint8_t)prefix[i])) {
            return false;
        }
    }
    return true;
}

bool mb_slice_eq_ci_lit(mb_slice s, const char *lit) {
    return s.len == strlen(lit) && mb_slice_has_prefix_ci_lit(s, lit);
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
