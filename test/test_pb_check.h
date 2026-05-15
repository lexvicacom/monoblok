#ifndef MB_TEST_PB_CHECK_H
#define MB_TEST_PB_CHECK_H

#include "test_check.h"

#include <string.h>

static void check_text(pb_slice s, const char *want) {
    CHECK(s.len == strlen(want));
    CHECK(memcmp(s.ptr, want, s.len) == 0);
}

#endif
