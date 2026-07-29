/* buf.h — checked allocation helpers and a growable byte buffer.
 *
 * Every allocation in the tree goes through zb_malloc/zb_realloc/zb_calloc so
 * that failure is a return value the caller propagates, never an abort (§2.1).
 */
#ifndef ZB_UTIL_BUF_H
#define ZB_UTIL_BUF_H

#include <stdarg.h>
#include <stddef.h>

/* Return NULL on failure. zb_malloc(0) returns a valid 1-byte block so that a
 * NULL return always means failure. */
void *zb_malloc(size_t size);
void *zb_calloc(size_t count, size_t size);
void *zb_realloc(void *ptr, size_t size);
void zb_free(void *ptr);

/* Duplicate a string. Caller frees with zb_free. NULL in -> NULL out. */
char *zb_strdup(const char *s);
char *zb_strndup(const char *s, size_t n);

/* A growable byte buffer that always keeps a NUL one past `len`, so `ptr` is
 * safe to treat as a C string when the content is text.
 *
 * `ptr` is NULL until the first successful append; use zb_buf_str() to read it
 * as a string without a NULL check. */
typedef struct {
    char *ptr;
    size_t len;
    size_t cap;
} zb_buf;

void zb_buf_init(zb_buf *b);
void zb_buf_free(zb_buf *b);

/* Ensure room for `extra` more bytes. 0 on success, -1 on OOM/overflow. */
int zb_buf_reserve(zb_buf *b, size_t extra);

/* All return 0 on success, -1 on OOM. On failure the buffer is unchanged and
 * still valid (and still needs freeing). */
int zb_buf_append(zb_buf *b, const void *data, size_t len);
int zb_buf_append_str(zb_buf *b, const char *s);
int zb_buf_append_char(zb_buf *b, char c);
int zb_buf_printf(zb_buf *b, const char *fmt, ...);
int zb_buf_vprintf(zb_buf *b, const char *fmt, va_list ap);

/* Never NULL: an empty buffer reads as "". */
const char *zb_buf_str(const zb_buf *b);

/* Hand the bytes to the caller (NUL-terminated, caller frees with zb_free) and
 * reset the buffer to empty. Returns NULL only if the buffer never allocated,
 * in which case a fresh empty string is returned instead — so NULL means OOM. */
char *zb_buf_detach(zb_buf *b);

/* Drop the contents but keep the allocation, for reuse across retries. */
void zb_buf_clear(zb_buf *b);

/* Formatted allocation. Caller frees with zb_free. NULL on OOM. */
char *zb_asprintf(const char *fmt, ...);

#endif /* ZB_UTIL_BUF_H */
