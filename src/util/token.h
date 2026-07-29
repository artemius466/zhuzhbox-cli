/* token.h — the extractToken() equivalent from vercel/js/manage.js.
 *
 * Deliberately hand-written: POSIX <regex.h> does not exist on MSVC, and this
 * is the only pattern matching the program needs. */
#ifndef ZB_UTIL_TOKEN_H
#define ZB_UTIL_TOKEN_H

/* Accepts a full URL ("https://zhuzhbox.fun/d/abc123XYZ"), a bare path
 * ("/d/abc123XYZ"), or a bare token ("abc123XYZ"), with any query string or
 * fragment ignored.
 *
 * Returns an owned token (caller zb_free) or NULL if the input does not
 * contain something shaped like a token, i.e. [A-Za-z0-9]{6,32}. */
char *zb_extract_token(const char *input);

/* 1 if `s` is exactly a well-formed token. */
int zb_is_token(const char *s);

#endif /* ZB_UTIL_TOKEN_H */
