/* str.h — bounded string helpers, UTF-8 measurement, filename sanitizing,
 * and a secure zero the compiler is not allowed to elide. */
#ifndef ZB_UTIL_STR_H
#define ZB_UTIL_STR_H

#include <stddef.h>

/* snprintf with the return value checked. Returns 0 if the whole string fit,
 * -1 on truncation or encoding error. The destination is always NUL-terminated
 * when `size` > 0. */
int zb_snprintf(char *dst, size_t size, const char *fmt, ...);

/* Case-insensitive compare over ASCII only (locale-independent, which is what
 * we want for header names and MIME types). */
int zb_streq_ci(const char *a, const char *b);
int zb_str_starts_with(const char *s, const char *prefix);
int zb_str_ends_with_ci(const char *s, const char *suffix);

/* Number of UTF-8 code points in `s`. Invalid bytes count as one each, so this
 * never under-counts a limit check. */
size_t zb_utf8_len(const char *s);

/* Byte offset of the start of code point `n`, or strlen(s) if there are fewer
 * than `n` code points. */
size_t zb_utf8_offset(const char *s, size_t n);

/* Terminal columns `s` occupies: combining marks 0, wide (CJK/emoji) 2,
 * everything else 1. Enough to keep a table aligned without linking ICU. */
size_t zb_display_width(const char *s);

/* Overwrite `n` bytes at `p` with zero in a way the optimizer must keep. */
void zb_secure_zero(void *p, size_t n);

/* zb_free a string after zeroing it. Safe on NULL. */
void zb_free_secret(char *s);

/* Turn a server-supplied filename into something safe to create inside a
 * target directory (§2.1): strips any path prefix, rejects "." and "..",
 * strips control characters, refuses Windows reserved device names and
 * trailing dots/spaces, and truncates to 200 bytes on a code point boundary.
 *
 * Returns an owned string (caller zb_free), or NULL on OOM. Never returns an
 * empty string — an input with nothing usable left yields "download". */
char *zb_sanitize_filename(const char *name);

/* Same rules, but for a directory name derived from a collection title. */
char *zb_sanitize_dirname(const char *name);

/* Percent-encode `s` for use in a URL path segment. Owned result, NULL on OOM. */
char *zb_url_escape_path(const char *s);

/* Trim ASCII whitespace in place, returning `s`. */
char *zb_str_trim(char *s);

#endif /* ZB_UTIL_STR_H */
