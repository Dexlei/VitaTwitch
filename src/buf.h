#ifndef VT_BUF_H
#define VT_BUF_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Growable byte buffer. */
typedef struct {
    uint8_t *data;
    size_t size;
    size_t cap;
} buf_t;

static inline void buf_reset(buf_t *b) { b->size = 0; }

static inline void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static inline int buf_reserve(buf_t *b, size_t need)
{
    if (need <= b->cap)
        return 0;
    size_t ncap = b->cap ? b->cap : 4096;
    while (ncap < need)
        ncap *= 2;
    uint8_t *n = realloc(b->data, ncap);
    if (!n)
        return -1;
    b->data = n;
    b->cap = ncap;
    return 0;
}

static inline int buf_append(buf_t *b, const void *src, size_t len)
{
    /* +1 keeps room for a NUL terminator so text bodies can be used as C strings. */
    if (buf_reserve(b, b->size + len + 1) < 0)
        return -1;
    memcpy(b->data + b->size, src, len);
    b->size += len;
    b->data[b->size] = 0;
    return 0;
}

#endif
