#include "buf.h"

#include <stdlib.h>
#include <string.h>

bool mb_buf_reserve(mb_buf *buf, size_t additional) {
    if (additional > SIZE_MAX - buf->len) {
        return false;
    }
    const size_t need = buf->len + additional;
    if (need <= buf->cap) {
        return true;
    }

    size_t next = buf->cap == 0 ? 1024 : buf->cap;
    while (next < need) {
        if (next > SIZE_MAX / 2) {
            next = need;
            break;
        }
        next *= 2;
    }

    uint8_t *new_ptr = realloc(buf->ptr, next);
    if (new_ptr == NULL) {
        return false;
    }
    buf->ptr = new_ptr;
    buf->cap = next;
    return true;
}

bool mb_buf_append(mb_buf *buf, const void *src, size_t len) {
    if (!mb_buf_reserve(buf, len)) {
        return false;
    }
    memcpy(buf->ptr + buf->len, src, len);
    buf->len += len;
    return true;
}

bool mb_buf_append_byte(mb_buf *buf, uint8_t byte) {
    if (!mb_buf_reserve(buf, 1)) {
        return false;
    }
    buf->ptr[buf->len] = byte;
    buf->len += 1;
    return true;
}

void mb_buf_consume(mb_buf *buf, size_t n) {
    if (n >= buf->len) {
        buf->len = 0;
        return;
    }
    memmove(buf->ptr, buf->ptr + n, buf->len - n);
    buf->len -= n;
}

void mb_buf_clear(mb_buf *buf) {
    buf->len = 0;
}

void mb_buf_free(mb_buf *buf) {
    free(buf->ptr);
    *buf = (mb_buf){0};
}

void mb_buf_swap(mb_buf *a, mb_buf *b) {
    const mb_buf tmp = *a;
    *a = *b;
    *b = tmp;
}

