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

// Short aliases: local sugar for pb_eval.c and the form fragments only. Other
// files include pb_eval_internal.h and use the pb_eval_* names.
#define ok pb_eval_ok
#define nil pb_eval_nil
#define fail pb_eval_fail
#define text_eq pb_eval_text_eq
#define truthy pb_eval_truthy
#define bool_value pb_eval_bool_value
#define as_number pb_eval_as_number
#define as_string pb_eval_as_string
#define coerce_payload pb_eval_coerce_payload
#define value_eq pb_eval_value_eq
#define state_slot pb_eval_state_slot
#define state_set_bytes pb_eval_state_set_bytes
#define state_set_emit_subject pb_eval_state_set_emit_subject
#define note_suppressed pb_eval_note_suppressed

typedef enum window_calc {
    WINDOW_SUM,
    WINDOW_AVG,
    WINDOW_MIN,
    WINDOW_MAX,
    WINDOW_VARIANCE,
    WINDOW_STDDEV,
} window_calc;

#ifndef PB_EVAL_C
static bool ring_push_time(pb_eval_state_entry *slot, double x, uint64_t t_ms);
static pb_eval_result calc_ring(const pb_eval_state_entry *slot, window_calc calc);
#endif

#endif
