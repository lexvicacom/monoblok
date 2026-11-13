#include "pb_regex.h"
#include "pb_regex_internal.h"

#include <stdalign.h>
#include <string.h>

// Eager DFA over the capture-free NFA with the unanchored prefix folded in
// (every pending set also holds pc 0), so one pass answers "matches anywhere".
//
// A state is (pending NFA pcs, previous-byte flag). Pending pcs are not yet
// epsilon-closed: the closure runs when the next byte is known, which makes ^ $
// \b \B exact (START and WORD flags, $ via accept_end). Construction gives up
// only on the PB_REGEX_DFA_* caps. ACCEPT means a match ended before the byte.

#define DFA_ACCEPT 0 // absorbing: a match was found
#define DFA_DEAD 1   // absorbing: no match is possible (anchored patterns only)
#define DFA_FIRST 2  // first real state id

enum { FLAG_START, FLAG_NONWORD, FLAG_WORD };

// Finished DFA: arena-owned, read-only. Tables hold row offsets (state * nclasses).
struct pb_rx_dfa {
    uint8_t classmap[256];
    uint16_t nclasses;
    uint16_t nstates;
    uint16_t start[3];   // row offsets of the initial states, indexed by FLAG_*
    uint16_t dead_off;   // row offset of DEAD; rows <= this are ACCEPT or DEAD
    uint16_t s0_off;     // row offset of the shared start state when skip_byte >= 0
    int skip_byte;       // the only byte that leaves the start state, or -1
    const uint16_t *trans;      // nstates * nclasses row offsets (state id * nclasses)
    const uint8_t *accept_end;  // per state id: matches if the text ends here
};

// Construction state; lives on the stack and in a temporary arena freed at the end.
typedef struct pb_dfa_build {
    const pb_rx_inst *prog;
    size_t nprog;
    bool anchored, has_bol, has_wordb;
    size_t nclasses;
    uint8_t classmap[256];
    uint8_t rep[256];    // first byte of each class
    size_t set_bytes;    // bytes per NFA-pc bitset
    size_t cap;          // maximum state count
    size_t nstates;
    uint16_t *trans;     // state ids during construction
    uint8_t *sets;       // pending bitset per state
    uint8_t *flags;
    uint32_t *hashes;
    uint8_t *accept_end;
    uint8_t *next;       // scratch bitset for the transition being built
    uint32_t *visited;
    uint32_t gen;
    uint16_t *stack;
} pb_dfa_build;

static bool bit_get(const uint8_t *set, size_t i) { return (set[i >> 3] >> (i & 7)) & 1u; }
static void bit_set(uint8_t *set, size_t i) { set[i >> 3] = (uint8_t)(set[i >> 3] | (1u << (i & 7))); }

static void refine_classes(pb_dfa_build *b, const uint8_t *set) {
    uint16_t remap[512];
    for (size_t i = 0; i < 512; i += 1) remap[i] = 0xFFFF;
    uint8_t next_map[256];
    uint16_t n = 0;
    for (unsigned c = 0; c < 256; c += 1) {
        size_t key = (size_t)b->classmap[c] * 2 + (pb_rx_set_has(set, c) ? 1 : 0);
        if (remap[key] == 0xFFFF) remap[key] = n++;
        next_map[c] = (uint8_t)remap[key];
    }
    memcpy(b->classmap, next_map, sizeof next_map);
    b->nclasses = n;
}

// Bytes the program never tells apart share a class (ids fit uint8_t).
static void build_classes(pb_dfa_build *b) {
    uint8_t word[RX_SET_BYTES] = {0};
    memset(b->classmap, 0, sizeof b->classmap);
    b->nclasses = 1;
    for (size_t pc = 0; pc < b->nprog; pc += 1) {
        const pb_rx_inst *in = &b->prog[pc];
        if (in->op == OP_CHAR) {
            uint8_t one[RX_SET_BYTES] = {0};
            one[in->byte >> 3] = (uint8_t)(1u << (in->byte & 7));
            refine_classes(b, one);
        } else if (in->op == OP_SET) {
            refine_classes(b, in->set);
        }
    }
    if (b->has_wordb) {
        for (unsigned c = 0; c < 256; c += 1) {
            if (pb_rx_is_word_byte((unsigned char)c)) word[c >> 3] = (uint8_t)(word[c >> 3] | (1u << (c & 7)));
        }
        refine_classes(b, word);
    }
    for (int c = 255; c >= 0; c -= 1) b->rep[b->classmap[c]] = (uint8_t)c;
}

// START matters only to ^, WORD only to \b, so unneeded distinctions collapse.
static uint8_t normalize_flag(const pb_dfa_build *b, uint8_t flag) {
    if (flag == FLAG_START) return b->has_bol ? FLAG_START : FLAG_NONWORD;
    if (flag == FLAG_WORD) return b->has_wordb ? FLAG_WORD : FLAG_NONWORD;
    return FLAG_NONWORD;
}

static uint32_t hash_state(const pb_dfa_build *b, const uint8_t *set, uint8_t flag) {
    uint32_t h = 2166136261u ^ flag;
    for (size_t i = 0; i < b->set_bytes; i += 1) {
        h = (h ^ set[i]) * 16777619u;
    }
    return h;
}

// Returns the state id for (set, flag), creating it, or -1 when the state cap is hit.
static int intern_state(pb_dfa_build *b, const uint8_t *set, uint8_t flag) {
    bool empty = true;
    for (size_t i = 0; i < b->set_bytes; i += 1) {
        if (set[i] != 0) empty = false;
    }
    if (empty) return DFA_DEAD;
    uint32_t h = hash_state(b, set, flag);
    for (size_t i = DFA_FIRST; i < b->nstates; i += 1) {
        if (b->hashes[i] == h && b->flags[i] == flag && memcmp(b->sets + i * b->set_bytes, set, b->set_bytes) == 0) {
            return (int)i;
        }
    }
    if (b->nstates >= b->cap) return -1;
    size_t id = b->nstates++;
    memcpy(b->sets + id * b->set_bytes, set, b->set_bytes);
    b->flags[id] = flag;
    b->hashes[id] = h;
    return (int)id;
}

// Closes state `id` over epsilon edges given the next byte (-1 at end of text).
// True as soon as MATCH is reachable; else leaves the stepped pcs in b->next.
static bool close_state(pb_dfa_build *b, size_t id, int byte) {
    const uint8_t *pending = b->sets + id * b->set_bytes;
    uint8_t flag = b->flags[id];
    size_t sp = 0;
    b->gen += 1;
    memset(b->next, 0, b->set_bytes);
    for (size_t pc = 0; pc < b->nprog; pc += 1) {
        if (bit_get(pending, pc)) b->stack[sp++] = (uint16_t)pc;
    }
    while (sp > 0) {
        size_t pc = b->stack[--sp];
        for (;;) {
            if (b->visited[pc] == b->gen) break;
            b->visited[pc] = b->gen;
            const pb_rx_inst *in = &b->prog[pc];
            if (in->op == OP_JMP) {
                pc = in->x;
            } else if (in->op == OP_SPLIT) {
                b->stack[sp++] = in->y;
                pc = in->x;
            } else if (in->op == OP_SAVE) {
                pc += 1;
            } else if (in->op == OP_BOL || in->op == OP_EOL || in->op == OP_WORDB || in->op == OP_NWORDB) {
                bool holds;
                if (in->op == OP_BOL) {
                    holds = flag == FLAG_START;
                } else if (in->op == OP_EOL) {
                    holds = byte < 0;
                } else {
                    bool before = flag == FLAG_WORD;
                    bool after = byte >= 0 && pb_rx_is_word_byte((unsigned char)byte);
                    holds = (before != after) == (in->op == OP_WORDB);
                }
                if (!holds) break;
                pc += 1;
            } else if (in->op == OP_MATCH) {
                return true;
            } else {
                bool consumes = byte >= 0 && (in->op == OP_CHAR ? in->byte == byte : pb_rx_set_has(in->set, (unsigned)byte));
                if (consumes) bit_set(b->next, pc + 1);
                break;
            }
        }
    }
    return false;
}

static bool build_row(pb_dfa_build *b, size_t id) {
    for (size_t k = 0; k < b->nclasses; k += 1) {
        int byte = b->rep[k];
        uint16_t target;
        if (close_state(b, id, byte)) {
            target = DFA_ACCEPT;
        } else {
            if (!b->anchored) bit_set(b->next, 0);
            int t = intern_state(b, b->next, normalize_flag(b, pb_rx_is_word_byte((unsigned char)byte) ? FLAG_WORD : FLAG_NONWORD));
            if (t < 0) return false;
            target = (uint16_t)t;
        }
        b->trans[id * b->nclasses + k] = target;
    }
    b->accept_end[id] = close_state(b, id, -1) ? 1 : 0;
    return true;
}

static bool dfa_build_states(pb_dfa_build *b, uint16_t start[3]) {
    for (size_t k = 0; k < b->nclasses; k += 1) {
        b->trans[DFA_ACCEPT * b->nclasses + k] = DFA_ACCEPT;
        b->trans[DFA_DEAD * b->nclasses + k] = DFA_DEAD;
    }
    b->accept_end[DFA_ACCEPT] = 1;
    b->accept_end[DFA_DEAD] = 0;
    b->nstates = DFA_FIRST;
    for (uint8_t f = 0; f < 3; f += 1) {
        memset(b->next, 0, b->set_bytes);
        if (!b->anchored || f == FLAG_START) bit_set(b->next, 0);
        int id = intern_state(b, b->next, normalize_flag(b, f));
        if (id < 0) return false;
        start[f] = (uint16_t)id;
    }
    for (size_t id = DFA_FIRST; id < b->nstates; id += 1) {
        if (!build_row(b, id)) return false;
    }
    return true;
}

pb_rx_dfa *pb_rx_dfa_build(pb_arena *arena, const pb_rx_inst *prog, size_t nprog, bool anchored) {
    pb_dfa_build b = {.prog = prog, .nprog = nprog, .anchored = anchored};
    for (size_t pc = 0; pc < nprog; pc += 1) {
        if (prog[pc].op == OP_BOL) b.has_bol = true;
        if (prog[pc].op == OP_WORDB || prog[pc].op == OP_NWORDB) b.has_wordb = true;
    }
    build_classes(&b);
    size_t max_entries = PB_REGEX_DFA_MAX_TABLE_BYTES / sizeof(uint16_t);
    b.cap = max_entries / b.nclasses;
    if (b.cap > PB_REGEX_DFA_MAX_STATES) b.cap = PB_REGEX_DFA_MAX_STATES;
    if (b.cap < 8) return NULL;
    b.set_bytes = (nprog + 8) / 8;

    pb_arena tmp = {0};
    pb_rx_dfa *result = NULL;
    pb_rx_dfa *d = NULL;
    uint16_t *trans = NULL;
    uint8_t *accept_end = NULL;
    uint16_t start[3];
    size_t entries = 0;
    b.trans = pb_arena_alloc(&tmp, b.cap * b.nclasses * sizeof *b.trans, alignof(uint16_t));
    b.sets = pb_arena_alloc(&tmp, b.cap * b.set_bytes, 1);
    b.flags = pb_arena_alloc(&tmp, b.cap, 1);
    b.hashes = pb_arena_alloc(&tmp, b.cap * sizeof *b.hashes, alignof(uint32_t));
    b.accept_end = pb_arena_alloc(&tmp, b.cap, 1);
    b.next = pb_arena_alloc(&tmp, b.set_bytes, 1);
    b.visited = pb_arena_alloc(&tmp, nprog * sizeof *b.visited, alignof(uint32_t));
    b.stack = pb_arena_alloc(&tmp, (2 * nprog + 2) * sizeof *b.stack, alignof(uint16_t));
    if (b.trans == NULL || b.sets == NULL || b.flags == NULL || b.hashes == NULL || b.accept_end == NULL || b.next == NULL || b.visited == NULL || b.stack == NULL) {
        goto done;
    }
    memset(b.visited, 0, nprog * sizeof *b.visited);
    memset(b.sets, 0, b.cap * b.set_bytes);
    if (!dfa_build_states(&b, start)) goto done;

    entries = b.nstates * b.nclasses;
    d = pb_arena_alloc(arena, sizeof *d, alignof(pb_rx_dfa));
    trans = pb_arena_alloc(arena, entries * sizeof *trans, alignof(uint16_t));
    accept_end = pb_arena_alloc(arena, b.nstates, 1);
    if (d == NULL || trans == NULL || accept_end == NULL) goto done;
    for (size_t i = 0; i < entries; i += 1) {
        trans[i] = (uint16_t)(b.trans[i] * b.nclasses);
    }
    memcpy(accept_end, b.accept_end, b.nstates);
    memcpy(d->classmap, b.classmap, sizeof d->classmap);
    d->nclasses = (uint16_t)b.nclasses;
    d->nstates = (uint16_t)b.nstates;
    d->dead_off = (uint16_t)(DFA_DEAD * b.nclasses);
    for (size_t f = 0; f < 3; f += 1) d->start[f] = (uint16_t)(start[f] * b.nclasses);
    d->trans = trans;
    d->accept_end = accept_end;
    d->skip_byte = -1;
    d->s0_off = 0;
    // A start state only one byte can leave lets memchr do the skipping.
    if (start[0] == start[1] && start[1] == start[2] && start[0] == DFA_FIRST) {
        size_t leaving = 0;
        int last = -1;
        for (unsigned c = 0; c < 256; c += 1) {
            if (trans[DFA_FIRST * b.nclasses + b.classmap[c]] != DFA_FIRST * b.nclasses) {
                leaving += 1;
                last = (int)c;
            }
        }
        if (leaving == 1) {
            d->skip_byte = last;
            d->s0_off = (uint16_t)(DFA_FIRST * b.nclasses);
        }
    }
    result = d;
done:
    pb_arena_free(&tmp);
    return result;
}

size_t pb_rx_dfa_state_count(const pb_rx_dfa *dfa) { return dfa->nstates; }

bool pb_rx_dfa_test_end(const pb_rx_dfa *d, pb_slice text, size_t from, size_t *end) {
    const uint16_t *t = d->trans;
    const uint8_t *cm = d->classmap;
    const unsigned char *p = (const unsigned char *)text.ptr;
    size_t len = text.len, pos = from;
    unsigned flag = from == 0 ? FLAG_START : (pb_rx_is_word_byte(p[from - 1]) ? FLAG_WORD : FLAG_NONWORD);
    unsigned s = d->start[flag];
    if (s <= d->dead_off) {
        *end = from;
        return s == 0 ? true : d->accept_end[s / d->nclasses] != 0;
    }
    if (d->skip_byte >= 0) {
        unsigned s0 = d->s0_off;
        for (;;) {
            if (s == s0) {
                if (pos >= len) break;
                const unsigned char *hit = memchr(p + pos, d->skip_byte, len - pos);
                if (hit == NULL) {
                    *end = len;
                    return d->accept_end[s / d->nclasses] != 0;
                }
                pos = (size_t)(hit - p);
            }
            while (pos < len) {
                s = t[s + cm[p[pos]]];
                pos += 1;
                if (s <= s0) break;
            }
            if (s == 0) {
                *end = pos - 1;
                return true;
            }
            if (s == d->dead_off) return false;
            if (s != s0) break; // ran off the end in a live state
        }
    } else {
        while (pos < len) {
            unsigned next = t[s + cm[p[pos]]];
            pos += 1;
            if (next == s) {
                // Self-loop: scan on without a dependent load chain.
                while (pos < len && t[s + cm[p[pos]]] == s) pos += 1;
                continue;
            }
            s = next;
            if (s <= d->dead_off) {
                *end = pos - 1;
                return s == 0;
            }
        }
    }
    *end = len;
    return d->accept_end[s / d->nclasses] != 0;
}
