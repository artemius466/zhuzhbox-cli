/* client.h — libcurl setup, URL building, and the one place an HTTP response
 * becomes either JSON or a zb_error carrying the server's own message.
 *
 * There is exactly one multi handle per client and no threads: concurrent
 * uploads are concurrent easy handles driven by one event loop (§2). */
#ifndef ZB_API_CLIENT_H
#define ZB_API_CLIENT_H

#include <curl/curl.h>
#include <stdint.h>
#include <stdio.h>

#include "cli.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/json.h"

typedef struct zb_client zb_client;

/* Per-transfer state. One of these is attached to every easy handle: it
 * accumulates the response, captures Retry-After, and drives progress. */
typedef struct {
    zb_buf body;       /* response body, unless `sink` is set */
    FILE *sink;        /* stream the body here instead of buffering it */
    long retry_after;  /* seconds from the Retry-After header, -1 if absent */
    int sink_failed;   /* a write to `sink` failed */

    /* Set before a ranged download. If the server answers 200 instead of 206
     * it ignored the Range, and appending its bytes to an existing .part file
     * would splice two different prefixes together; the transfer is aborted
     * and `restart_needed` is set so the caller can start over cleanly. */
    int require_partial;
    int restart_needed;

    /* An error response must not be written into the download file — it is
     * diverted into `body` so the server's message can still be reported. */
    int status_checked;
    int divert_to_buffer;
    CURL *easy; /* borrowed, so the callbacks can read the status code */

    /* Called at most ~10x/second with the running byte counts. */
    void (*on_progress)(void *ctx, uint64_t now, uint64_t total);
    void *progress_ctx;
    int uploading; /* report the upload counters rather than the download ones */

    /* Bytes already accounted for before this transfer started, so a chunked
     * upload can report progress across the whole file rather than per chunk. */
    uint64_t base_offset;
} zb_xfer;

void zb_xfer_init(zb_xfer *x);
void zb_xfer_free(zb_xfer *x);
void zb_xfer_reset(zb_xfer *x); /* keep the allocation, drop the contents */

/* NULL on OOM. Holds a borrowed pointer to `opt`, which must outlive it. */
zb_client *zb_client_new(const zb_options *opt, zb_error *err);
void zb_client_free(zb_client *c);

CURLM *zb_client_multi(zb_client *c);
const zb_options *zb_client_options(const zb_client *c);

/* "<api base><path>". `path` starts with '/'. Owned result. */
char *zb_client_url(const zb_client *c, const char *path);

/* A preconfigured easy handle: timeouts, no signals, interrupt-aware progress,
 * redaction-aware debug output, and `x` wired to the write/header/progress
 * callbacks. Returns NULL on failure. Free with curl_easy_cleanup(). */
CURL *zb_client_easy(zb_client *c, zb_xfer *x, zb_error *err);

/* Attach a JSON request body. The handle keeps a copy, so `body` may be freed
 * afterwards. Sets the method to POST unless `method` says otherwise. */
zb_status zb_client_set_json_body(CURL *easy, const zb_json *body,
                                  struct curl_slist **headers, zb_error *err);

/* Turn a finished transfer into a result.
 *
 * On a 2xx, parses the body as JSON into *out (may be NULL if the caller does
 * not want it; a 204 yields NULL). On anything else, fills `err` with the
 * server's `error` string verbatim plus whatever structured fields the body
 * carried (quota numbers, chunk counts) and the Retry-After header. */
zb_status zb_client_finish(const zb_client *c, CURL *easy, zb_xfer *x,
                           const char *url, zb_json **out, long *out_status,
                           zb_error *err);

/* Blocking convenience wrapper: build, perform, finish, clean up.
 *
 * `extra_headers` is a NULL-terminated array of "Name: value" strings, or NULL.
 * `out` and `out_status` may each be NULL. */
zb_status zb_client_request(zb_client *c, const char *method, const char *path,
                            const zb_json *body,
                            const char *const *extra_headers, zb_json **out,
                            long *out_status, zb_error *err);

/* Map a libcurl result to a zb_error, honoring the interrupt flag so that a
 * callback-aborted transfer reads as "canceled" rather than a network fault. */
zb_status zb_client_curl_error(CURLcode code, const char *url, zb_error *err);

#endif /* ZB_API_CLIENT_H */
