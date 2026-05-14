#ifndef MB_BUF_H
#define MB_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Growable byte buffer used for protocol input/output staging.
typedef struct mb_buf {
    uint8_t *ptr;
    size_t len;
    size_t cap;
} mb_buf;

bool mb_buf_reserve(mb_buf *buf, size_t additional);
bool mb_buf_append(mb_buf *buf, const void *src, size_t len);
bool mb_buf_append_byte(mb_buf *buf, uint8_t byte);
void mb_buf_consume(mb_buf *buf, size_t n);
void mb_buf_clear(mb_buf *buf);
void mb_buf_free(mb_buf *buf);
void mb_buf_swap(mb_buf *a, mb_buf *b);

#endif
