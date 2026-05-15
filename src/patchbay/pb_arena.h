#ifndef PB_ARENA_H
#define PB_ARENA_H

#include <stddef.h>

// Small bump allocator used by patchbay parsing/evaluation code.
//
// The arena owns a linked list of blocks. Each allocation bumps the `used`
// cursor in one block; individual allocations are never freed. This keeps parse
// trees and temporary evaluation objects cheap to allocate and simple to tear
// down. Call pb_arena_reset() to reuse existing blocks for another short-lived
// batch, or pb_arena_free() to release all memory owned by the arena.
//
// Ownership rule: pointers returned by pb_arena_alloc()/pb_arena_memdup() remain
// valid until the next reset/free of the same arena. Do not free them directly.
typedef struct pb_arena_block {
    struct pb_arena_block *next;
    size_t used;
    size_t cap;
    unsigned char data[];
} pb_arena_block;

typedef struct pb_arena {
    pb_arena_block *head;
} pb_arena;

void *pb_arena_alloc(pb_arena *arena, size_t size, size_t align);
void *pb_arena_memdup(pb_arena *arena, const void *src, size_t len);
void pb_arena_reset(pb_arena *arena);
void pb_arena_free(pb_arena *arena);

#endif
