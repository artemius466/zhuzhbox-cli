/* error.h — the one error type. Everything is propagated by return value;
 * there is no global error state and no errno leaking into user-facing text. */
#ifndef ZB_UTIL_ERROR_H
#define ZB_UTIL_ERROR_H

#include <stdint.h>

typedef enum {
    ZB_OK = 0,
    ZB_ERR_USAGE,
    ZB_ERR_IO,
    ZB_ERR_NET,
    ZB_ERR_HTTP,
    ZB_ERR_PROTO,
    ZB_ERR_NOMEM,
    ZB_ERR_CANCELED
} zb_status;

/* Carries the HTTP status and the server's own `error` string, which is
 * written to be read by a person and must be surfaced verbatim (§9).
 *
 * `message` is owned by the struct; zb_error_clear() frees it. A zb_error is
 * always cleared by whoever declared it, on every path. */
typedef struct {
    zb_status status;
    long http_status; /* 0 when the failure was not an HTTP response */
    char *message;    /* owned; may be NULL */

    /* Parsed out of the response body when present, so a 429 can be reported
     * as quota exhaustion rather than as a rate limit (§9). */
    int has_quota;
    uint64_t used_bytes;
    uint64_t limit_bytes;
    uint64_t remaining_bytes;
    int window_days;

    /* From a 409 on /complete: how many chunks the server actually has. */
    int has_chunk_counts;
    uint64_t received_chunks;
    uint64_t total_chunks;

    /* From the Retry-After response header on a 503. -1 when absent. */
    long retry_after_seconds;
} zb_error;

void zb_error_init(zb_error *err);
void zb_error_clear(zb_error *err);

/* Take ownership of the fields of `src`, leaving `src` cleared. Used to hand
 * an error up through a layer that owns its own zb_error. */
void zb_error_move(zb_error *dst, zb_error *src);

/* Set status and a formatted message, replacing anything already there.
 * Always returns `status`, so callers can `return zb_error_setf(...)`. */
zb_status zb_error_setf(zb_error *err, zb_status status, const char *fmt, ...);

/* Set from an out-of-memory condition. Always returns ZB_ERR_NOMEM. */
zb_status zb_error_nomem(zb_error *err);

/* The message to show the user; never NULL. */
const char *zb_error_message(const zb_error *err);

/* Short machine-ish name for the status, used in --json and --debug output. */
const char *zb_status_name(zb_status status);

/* Map a libcurl easy result to a user-facing sentence. Returns NULL for
 * CURLE_OK. The string is static — do not free it.
 *
 * Declared as `int` rather than CURLcode so that only api/ has to include
 * curl.h; pass the CURLcode straight in. */
const char *zb_curl_message(int curl_code);

#endif /* ZB_UTIL_ERROR_H */
