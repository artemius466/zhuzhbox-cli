#include "util/buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *zb_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return malloc(size);
}

void *zb_calloc(size_t count, size_t size)
{
    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }
    /* calloc checks the product itself, but be explicit so 32-bit builds cannot
     * be talked into a small allocation by a huge count. */
    if (count > (size_t)-1 / size) {
        return NULL;
    }
    return calloc(count, size);
}

void *zb_realloc(void *ptr, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return realloc(ptr, size);
}

void zb_free(void *ptr)
{
    free(ptr);
}

char *zb_strdup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return zb_strndup(s, strlen(s));
}

char *zb_strndup(const char *s, size_t n)
{
    char *out;
    if (s == NULL) {
        return NULL;
    }
    if (n == (size_t)-1) {
        return NULL;
    }
    out = zb_malloc(n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

void zb_buf_init(zb_buf *b)
{
    b->ptr = NULL;
    b->len = 0;
    b->cap = 0;
}

void zb_buf_free(zb_buf *b)
{
    if (b == NULL) {
        return;
    }
    zb_free(b->ptr);
    zb_buf_init(b);
}

void zb_buf_clear(zb_buf *b)
{
    b->len = 0;
    if (b->ptr != NULL) {
        b->ptr[0] = '\0';
    }
}

int zb_buf_reserve(zb_buf *b, size_t extra)
{
    size_t need;
    size_t cap;
    char *grown;

    /* +1 for the trailing NUL we always keep. */
    if (extra > (size_t)-1 - 1 - b->len) {
        return -1;
    }
    need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }

    cap = b->cap != 0 ? b->cap : 64;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    grown = zb_realloc(b->ptr, cap);
    if (grown == NULL) {
        return -1;
    }
    b->ptr = grown;
    b->cap = cap;
    return 0;
}

int zb_buf_append(zb_buf *b, const void *data, size_t len)
{
    if (len == 0) {
        /* Still materialize the buffer so zb_buf_str() has something. */
        if (zb_buf_reserve(b, 0) != 0) {
            return -1;
        }
        b->ptr[b->len] = '\0';
        return 0;
    }
    if (zb_buf_reserve(b, len) != 0) {
        return -1;
    }
    memcpy(b->ptr + b->len, data, len);
    b->len += len;
    b->ptr[b->len] = '\0';
    return 0;
}

int zb_buf_append_str(zb_buf *b, const char *s)
{
    if (s == NULL) {
        return 0;
    }
    return zb_buf_append(b, s, strlen(s));
}

int zb_buf_append_char(zb_buf *b, char c)
{
    return zb_buf_append(b, &c, 1);
}

int zb_buf_vprintf(zb_buf *b, const char *fmt, va_list ap)
{
    va_list copy;
    int needed;
    size_t want;

    va_copy(copy, ap);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return -1;
    }
    want = (size_t)needed;
    if (zb_buf_reserve(b, want) != 0) {
        return -1;
    }
    /* cap - len is guaranteed > want by reserve(). */
    needed = vsnprintf(b->ptr + b->len, b->cap - b->len, fmt, ap);
    if (needed < 0) {
        b->ptr[b->len] = '\0';
        return -1;
    }
    b->len += want;
    b->ptr[b->len] = '\0';
    return 0;
}

int zb_buf_printf(zb_buf *b, const char *fmt, ...)
{
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = zb_buf_vprintf(b, fmt, ap);
    va_end(ap);
    return rc;
}

const char *zb_buf_str(const zb_buf *b)
{
    return b->ptr != NULL ? b->ptr : "";
}

char *zb_buf_detach(zb_buf *b)
{
    char *out = b->ptr;
    if (out == NULL) {
        out = zb_strdup("");
        if (out == NULL) {
            return NULL;
        }
    }
    zb_buf_init(b);
    return out;
}

char *zb_asprintf(const char *fmt, ...)
{
    zb_buf b;
    va_list ap;
    int rc;

    zb_buf_init(&b);
    va_start(ap, fmt);
    rc = zb_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (rc != 0) {
        zb_buf_free(&b);
        return NULL;
    }
    return zb_buf_detach(&b);
}
