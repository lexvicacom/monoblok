#ifndef PB_REGEX_INTERNAL_H
#define PB_REGEX_INTERNAL_H

// Internal to pb_regex.c and pb_regex_dfa.c: NFA program format and DFA entry points.

#include "pb_regex.h"
#include "pb_sexpr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RX_SET_BYTES 32 // 256-bit class bitmap

typedef enum pb_rx_op {
    OP_CHAR,
    OP_SET,
    OP_SPLIT, // try x first, then y
    OP_JMP,
    OP_SAVE,
    OP_BOL,
    OP_EOL,
    OP_WORDB,
    OP_NWORDB,
    OP_MATCH,
} pb_rx_op;

// One NFA instruction; `set` points at an arena bitmap owned by the regex.
typedef struct pb_rx_inst {
    uint8_t op;
    uint8_t byte;
    uint16_t x;
    uint16_t y;
    const uint8_t *set;
} pb_rx_inst;

static inline bool pb_rx_set_has(const uint8_t *set, unsigned c) {
    return (set[c >> 3] >> (c & 7)) & 1u;
}

static inline bool pb_rx_is_word_byte(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Eager capture-free DFA: arena-owned, read-only after construction.
typedef struct pb_rx_dfa pb_rx_dfa;

// NULL when a PB_REGEX_DFA_* cap is exceeded or the arena is exhausted (use the VM).
pb_rx_dfa *pb_rx_dfa_build(pb_arena *arena, const pb_rx_inst *prog, size_t nprog, bool anchored);
// True when prog matches in text[from..] (^ and \b see the whole text). On a hit
// *end is the earliest end offset of any match; unspecified on a miss.
bool pb_rx_dfa_test_end(const pb_rx_dfa *dfa, pb_slice text, size_t from, size_t *end);
size_t pb_rx_dfa_state_count(const pb_rx_dfa *dfa);

// TEST ONLY hooks: force the Pike VM path (irreversible), turn off the first-byte
// table or the DFA-bounded VM start window, and set the visited generation.
void pb_regex_disable_dfa(pb_regex *re);
void pb_regex_disable_first_set(pb_regex *re);
void pb_regex_disable_window(pb_regex *re);
void pb_regex_set_generation(pb_regex *re, uint32_t gen);

#endif
