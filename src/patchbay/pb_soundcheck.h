#ifndef PB_SOUNDCHECK_H
#define PB_SOUNDCHECK_H

#include <stdbool.h>
#include <stdint.h>

#define PB_SOUNDCHECK_DEFAULT_LINGER_MS UINT64_C(10000)

// CLI soundcheck knobs; input is always SUBJECT|payload rows on stdin.
typedef struct pb_soundcheck_options {
    uint64_t linger_ms;
    bool label;
} pb_soundcheck_options;

int pb_soundcheck_run(const char *path, pb_soundcheck_options opts);

#endif
