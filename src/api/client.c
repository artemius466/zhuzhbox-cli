#include "api/client.h"

#include <stdlib.h>
#include <string.h>

#include "util/platform.h"
#include "util/str.h"

struct zb_client {
    const zb_options *opt; /* borrowed */
    CURLM *multi;
    char *user_agent;
};

void zb_xfer_init(zb_xfer *x)
{
    memset(x, 0, sizeof(*x));
    zb_buf_init(&x->body);
    x->retry_after = -1;
}

void zb_xfer_free(zb_xfer *x)
{
    if (x == NULL) {
        return;
    }
    zb_buf_free(&x->body);
    memset(x, 0, sizeof(*x));
    x->retry_after = -1;
}

void zb_xfer_reset(zb_xfer *x)
{
    zb_buf_clear(&x->body);
    x->retry_after = -1;
    x->sink_failed = 0;
    x->status_checked = 0;
    x->divert_to_buffer = 0;
    x->restart_needed = 0;
    x->easy = NULL;
}

/* ------------------------------------------------------------------ */
/* libcurl callbacks                                                    */
/* ------------------------------------------------------------------ */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    zb_xfer *x = userdata;
    size_t total;

    /* size is always 1 in practice, but the multiply is the documented
     * contract and overflowing it would be a heap overflow. */
    if (size != 0 && nmemb > (size_t)-1 / size) {
        return 0;
    }
    total = size * nmemb;

    if (x->sink != NULL) {
        if (!x->status_checked) {
            long code = 0;
            x->status_checked = 1;
            if (x->easy != NULL) {
                curl_easy_getinfo(x->easy, CURLINFO_RESPONSE_CODE, &code);
            }
            if (code >= 400) {
                /* Keep the server's error out of the user's file, but keep it
                 * readable so the message can be surfaced verbatim. */
                x->divert_to_buffer = 1;
            } else if (x->require_partial && code == 200) {
                x->restart_needed = 1;
                return 0;
            }
        }
        if (!x->divert_to_buffer) {
            if (total > 0 && fwrite(ptr, 1, total, x->sink) != total) {
                x->sink_failed = 1;
                return 0; /* tells libcurl to fail with CURLE_WRITE_ERROR */
            }
            return total;
        }
    }
    if (zb_buf_append(&x->body, ptr, total) != 0) {
        return 0;
    }
    return total;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems,
                        void *userdata)
{
    zb_xfer *x = userdata;
    size_t total;
    static const char k_retry_after[] = "retry-after:";
    size_t name_len = sizeof(k_retry_after) - 1;

    if (size != 0 && nitems > (size_t)-1 / size) {
        return 0;
    }
    total = size * nitems;

    /* The 503 on /upload/init carries its wait in the header, not the body. */
    if (total > name_len) {
        size_t i;
        int match = 1;
        for (i = 0; i < name_len; i++) {
            char c = buffer[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (c != k_retry_after[i]) {
                match = 0;
                break;
            }
        }
        if (match) {
            char value[32];
            size_t n = total - name_len;
            if (n >= sizeof(value)) {
                n = sizeof(value) - 1;
            }
            memcpy(value, buffer + name_len, n);
            value[n] = '\0';
            {
                char *end = NULL;
                long parsed = strtol(zb_str_trim(value), &end, 10);
                if (end != value && parsed >= 0) {
                    x->retry_after = parsed;
                }
            }
        }
    }
    return total;
}

static int xferinfo_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    zb_xfer *x = userdata;

    /* This is the only thing that observes the interrupt flag during a
     * transfer; returning nonzero aborts it and the normal unwind path does
     * the rest */
    if (zb_interrupted) {
        return 1;
    }
    if (x != NULL && x->on_progress != NULL) {
        curl_off_t now = x->uploading ? ulnow : dlnow;
        curl_off_t total = x->uploading ? ultotal : dltotal;
        x->on_progress(x->progress_ctx,
                       x->base_offset + (now > 0 ? (uint64_t)now : 0),
                       total > 0 ? (uint64_t)total : 0);
    }
    return 0;
}

/* Redact anything that looks like a delete token before it reaches the debug
 * log. --debug must never make a capability secret grep-able in a terminal
 * scrollback or a pasted bug report */
static void debug_write_redacted(const char *data, size_t len)
{
    static const char *const k_secrets[] = {"x-delete-token:", "deletetoken"};
    size_t i = 0;

    while (i < len) {
        size_t s;
        int matched = 0;

        for (s = 0; s < sizeof(k_secrets) / sizeof(k_secrets[0]); s++) {
            size_t sl = strlen(k_secrets[s]);
            size_t k;
            if (i + sl > len) {
                continue;
            }
            for (k = 0; k < sl; k++) {
                char c = data[i + k];
                if (c >= 'A' && c <= 'Z') {
                    c = (char)(c - 'A' + 'a');
                }
                if (c != k_secrets[s][k]) {
                    break;
                }
            }
            if (k != sl) {
                continue;
            }
            /* Print the key, then swallow the value up to the next line break
             * or JSON delimiter. */
            fwrite(data + i, 1, sl, stderr);
            fputs(" <redacted>", stderr);
            i += sl;
            while (i < len && data[i] != '\n' && data[i] != '\r' &&
                   data[i] != ',' && data[i] != '}') {
                i++;
            }
            matched = 1;
            break;
        }
        if (!matched) {
            fputc(data[i], stderr);
            i++;
        }
    }
}

static int debug_cb(CURL *handle, curl_infotype type, char *data, size_t size,
                    void *userdata)
{
    (void)handle;
    (void)userdata;

    switch (type) {
    case CURLINFO_TEXT:
        fputs("* ", stderr);
        debug_write_redacted(data, size);
        break;
    case CURLINFO_HEADER_OUT:
        fputs("> ", stderr);
        debug_write_redacted(data, size);
        break;
    case CURLINFO_HEADER_IN:
        fputs("< ", stderr);
        debug_write_redacted(data, size);
        break;
    default:
        /* Never dump payload bytes: they are the user's file. */
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Client                                                               */
/* ------------------------------------------------------------------ */

zb_client *zb_client_new(const zb_options *opt, zb_error *err)
{
    zb_client *c = zb_calloc(1, sizeof(*c));
    if (c == NULL) {
        zb_error_nomem(err);
        return NULL;
    }
    c->opt = opt;
    c->multi = curl_multi_init();
    if (c->multi == NULL) {
        zb_free(c);
        zb_error_setf(err, ZB_ERR_NET, "could not initialize libcurl");
        return NULL;
    }
    c->user_agent = zb_asprintf("zhuzhbox-cli/%s", ZB_VERSION);
    if (c->user_agent == NULL) {
        curl_multi_cleanup(c->multi);
        zb_free(c);
        zb_error_nomem(err);
        return NULL;
    }
    return c;
}

void zb_client_free(zb_client *c)
{
    if (c == NULL) {
        return;
    }
    if (c->multi != NULL) {
        curl_multi_cleanup(c->multi);
    }
    zb_free(c->user_agent);
    zb_free(c);
}

CURLM *zb_client_multi(zb_client *c)
{
    return c->multi;
}

const zb_options *zb_client_options(const zb_client *c)
{
    return c->opt;
}

char *zb_client_url(const zb_client *c, const char *path)
{
    return zb_asprintf("%s%s", zb_opt_api(c->opt), path);
}

CURL *zb_client_easy(zb_client *c, zb_xfer *x, zb_error *err)
{
    CURL *easy = curl_easy_init();
    if (easy == NULL) {
        zb_error_setf(err, ZB_ERR_NET, "could not create an HTTP handle");
        return NULL;
    }
    x->easy = easy;

    curl_easy_setopt(easy, CURLOPT_USERAGENT, c->user_agent);
    /* libcurl must not install its own signal handlers; ours are the only
     * ones, and they do nothing but set a flag. */
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 15L);
    /* Deliberately no CURLOPT_TIMEOUT: a 25 GB upload is allowed to take
     * hours. A stall is detected by throughput instead (§9). */
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);

    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, x);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, x);

    if (c->opt->debug) {
        curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, debug_cb);
    }
    return easy;
}

zb_status zb_client_set_json_body(CURL *easy, const zb_json *body,
                                  struct curl_slist **headers, zb_error *err)
{
    char *text = zb_json_print(body, 0);
    struct curl_slist *grown;

    if (text == NULL) {
        return zb_error_nomem(err);
    }
    /* COPYPOSTFIELDS makes libcurl own a copy, so `text` can go away here. */
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(text));
    curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, text);
    zb_free(text);

    grown = curl_slist_append(*headers, "Content-Type: application/json");
    if (grown == NULL) {
        return zb_error_nomem(err);
    }
    *headers = grown;
    return ZB_OK;
}

zb_status zb_client_curl_error(CURLcode code, const char *url, zb_error *err)
{
    const char *message;

    if (code == CURLE_OK) {
        return ZB_OK;
    }
    /* A transfer we aborted ourselves is a cancellation, not a network fault —
     * and it maps to exit code 130, not 1. */
    if (code == CURLE_ABORTED_BY_CALLBACK && zb_interrupted) {
        return zb_error_setf(err, ZB_ERR_CANCELED, "interrupted");
    }
    message = zb_curl_message((int)code);
    err->http_status = 0;
    return zb_error_setf(err, ZB_ERR_NET, "%s (%s)",
                         message != NULL ? message : "the transfer failed", url);
}

zb_status zb_client_finish(const zb_client *c, CURL *easy, zb_xfer *x,
                           const char *url, zb_json **out, long *out_status,
                           zb_error *err)
{
    long status = 0;
    zb_json *parsed = NULL;

    (void)c;
    if (out != NULL) {
        *out = NULL;
    }
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
    if (out_status != NULL) {
        *out_status = status;
    }

    if (x->body.len > 0) {
        parsed = zb_json_parse(x->body.ptr, x->body.len);
    }

    if (status >= 200 && status < 300) {
        if (out != NULL) {
            *out = parsed;
        } else {
            zb_json_free(parsed);
        }
        return ZB_OK;
    }

    err->http_status = status;
    err->retry_after_seconds = x->retry_after;

    if (parsed != NULL && zb_json_is_object(parsed)) {
        const char *message = NULL;

        /* Structured fields first: a 429 means two very different things
         * depending on whether the body carries quota numbers (§9). */
        if (zb_json_get_u64(parsed, "usedBytes", &err->used_bytes) &&
            zb_json_get_u64(parsed, "limitBytes", &err->limit_bytes)) {
            err->has_quota = 1;
            (void)zb_json_get_u64(parsed, "remainingBytes",
                                  &err->remaining_bytes);
            {
                int64_t days = 0;
                if (zb_json_get_i64(parsed, "windowDays", &days) && days > 0 &&
                    days < 3650) {
                    err->window_days = (int)days;
                }
            }
        }
        if (zb_json_get_u64(parsed, "receivedChunks", &err->received_chunks) &&
            zb_json_get_u64(parsed, "totalChunks", &err->total_chunks)) {
            err->has_chunk_counts = 1;
        }

        if (zb_json_get_str(parsed, "error", &message) && message[0] != '\0') {
            /* Verbatim: the server's error strings are written for people. */
            zb_error_setf(err, ZB_ERR_HTTP, "%s", message);
            zb_json_free(parsed);
            return ZB_ERR_HTTP;
        }
    }
    zb_json_free(parsed);

    /* No JSON, or no `error` key: name the status and the URL rather than
     * saying "request failed". */
    return zb_error_setf(err, ZB_ERR_HTTP, "HTTP %ld from %s", status, url);
}

zb_status zb_client_request(zb_client *c, const char *method, const char *path,
                            const zb_json *body,
                            const char *const *extra_headers, zb_json **out,
                            long *out_status, zb_error *err)
{
    CURL *easy = NULL;
    struct curl_slist *headers = NULL;
    zb_xfer xfer;
    char *url = NULL;
    CURLcode code;
    zb_status rc;

    zb_xfer_init(&xfer);

    url = zb_client_url(c, path);
    if (url == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    easy = zb_client_easy(c, &xfer, err);
    if (easy == NULL) {
        rc = err->status;
        goto cleanup;
    }
    curl_easy_setopt(easy, CURLOPT_URL, url);

    if (extra_headers != NULL) {
        size_t i;
        for (i = 0; extra_headers[i] != NULL; i++) {
            struct curl_slist *grown = curl_slist_append(headers,
                                                         extra_headers[i]);
            if (grown == NULL) {
                rc = zb_error_nomem(err);
                goto cleanup;
            }
            headers = grown;
        }
    }

    if (body != NULL) {
        rc = zb_client_set_json_body(easy, body, &headers, err);
        if (rc != ZB_OK) {
            goto cleanup;
        }
    }

    if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        if (body == NULL) {
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
            curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, "");
        }
    } else {
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method);
    }

    if (headers != NULL) {
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    }

    code = curl_easy_perform(easy);
    if (code != CURLE_OK) {
        rc = zb_client_curl_error(code, url, err);
        goto cleanup;
    }

    rc = zb_client_finish(c, easy, &xfer, url, out, out_status, err);

cleanup:
    if (easy != NULL) {
        curl_easy_cleanup(easy);
    }
    curl_slist_free_all(headers);
    zb_free(url);
    zb_xfer_free(&xfer);
    return rc;
}
