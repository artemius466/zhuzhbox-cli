#include "util/token.h"

#include <string.h>

#include "util/buf.h"

#define ZB_TOKEN_MIN 6
#define ZB_TOKEN_MAX 32

static int is_token_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

int zb_is_token(const char *s)
{
    size_t n = 0;
    if (s == NULL) {
        return 0;
    }
    for (n = 0; s[n] != '\0'; n++) {
        if (!is_token_char(s[n])) {
            return 0;
        }
        if (n >= ZB_TOKEN_MAX) {
            return 0;
        }
    }
    return n >= ZB_TOKEN_MIN && n <= ZB_TOKEN_MAX;
}

char *zb_extract_token(const char *input)
{
    const char *p;
    const char *end;
    const char *seg_start;
    const char *best = NULL;
    size_t best_len = 0;

    if (input == NULL) {
        return NULL;
    }

    /* Skip leading whitespace. */
    p = input;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }

    /* Drop the scheme and authority, if any, so only the path is scanned. */
    {
        const char *scheme = strstr(p, "://");
        if (scheme != NULL) {
            const char *slash = strchr(scheme + 3, '/');
            if (slash == NULL) {
                return NULL; /* a bare host carries no token */
            }
            p = slash;
        }
    }

    /* Cut at the query string, fragment, or trailing whitespace. */
    end = p;
    while (*end != '\0' && *end != '?' && *end != '#' && *end != ' ' &&
           *end != '\t' && *end != '\r' && *end != '\n') {
        end++;
    }

    /* Take the last path segment that is shaped like a token. Scanning all of
     * them rather than only the final one means a trailing slash, an
     * "?inline=1" already stripped above, or an extra path element does not
     * defeat the match. */
    seg_start = p;
    for (;;) {
        if (p == end || *p == '/' || *p == '\\') {
            size_t len = (size_t)(p - seg_start);
            if (len >= ZB_TOKEN_MIN && len <= ZB_TOKEN_MAX) {
                size_t i;
                int ok = 1;
                for (i = 0; i < len; i++) {
                    if (!is_token_char(seg_start[i])) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) {
                    best = seg_start;
                    best_len = len;
                }
            }
            if (p == end) {
                break;
            }
            seg_start = p + 1;
        }
        p++;
    }

    if (best == NULL) {
        return NULL;
    }
    return zb_strndup(best, best_len);
}
