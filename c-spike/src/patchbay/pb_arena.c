#include "pb_arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t align_up(size_t n, size_t align) {
    const size_t mask = align - 1;
    return (n + mask) & ~mask;
}

void *pb_arena_alloc(pb_arena *arena, size_t size, size_t align) {
    if (align == 0) {
        align = sizeof(void *);
    }
    if ((align & (align - 1)) != 0) {
        return NULL;
    }

    for (pb_arena_block *b = arena->head; b != NULL; b = b->next) {
        const size_t off = align_up(b->used, align);
        if (off <= b->cap && size <= b->cap - off) {
            b->used = off + size;
            return b->data + off;
        }
    }

    const size_t min_cap = size + align;
    size_t cap = 4096;
    while (cap < min_cap) {
        cap *= 2;
    }

    pb_arena_block *b = malloc(sizeof *b + cap);
    if (b == NULL) {
        return NULL;
    }
    b->next = arena->head;
    b->used = 0;
    b->cap = cap;
    arena->head = b;

    const size_t off = align_up(b->used, align);
    b->used = off + size;
    return b->data + off;
}

void *pb_arena_memdup(pb_arena *arena, const void *src, size_t len) {
    void *dst = pb_arena_alloc(arena, len == 0 ? 1 : len, 1);
    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, len);
    return dst;
}

void pb_arena_reset(pb_arena *arena) {
    for (pb_arena_block *b = arena->head; b != NULL; b = b->next) {
        b->used = 0;
    }
}

void pb_arena_free(pb_arena *arena) {
    pb_arena_block *b = arena->head;
    while (b != NULL) {
        pb_arena_block *next = b->next;
        free(b);
        b = next;
    }
    arena->head = NULL;
}
