#ifndef PB_FORM_CHUNK_H
#define PB_FORM_CHUNK_H

#include "pb_eval_internal.h"

#if PB_ENABLE_JSON
#include "yyjson.h"
#endif

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum window_calc {
    WINDOW_SUM,
    WINDOW_AVG,
    WINDOW_MIN,
    WINDOW_MAX,
    WINDOW_VARIANCE,
    WINDOW_STDDEV,
} window_calc;

static bool ring_push_time(pb_eval_state_entry *slot, double x, uint64_t t_ms);
static pb_eval_result calc_ring(const pb_eval_state_entry *slot, window_calc calc);

#endif
