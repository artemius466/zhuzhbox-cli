#include "util/error.h"

#include <curl/curl.h>
#include <stdarg.h>
#include <string.h>

#include "util/buf.h"

void zb_error_init(zb_error *err)
{
    memset(err, 0, sizeof(*err));
    err->status = ZB_OK;
    err->retry_after_seconds = -1;
}

void zb_error_clear(zb_error *err)
{
    if (err == NULL) {
        return;
    }
    zb_free(err->message);
    zb_error_init(err);
}

void zb_error_move(zb_error *dst, zb_error *src)
{
    if (dst == src) {
        return;
    }
    zb_error_clear(dst);
    *dst = *src;
    zb_error_init(src);
}

zb_status zb_error_setf(zb_error *err, zb_status status, const char *fmt, ...)
{
    zb_buf b;
    va_list ap;
    int rc;

    if (err == NULL) {
        return status;
    }
    zb_buf_init(&b);
    va_start(ap, fmt);
    rc = zb_buf_vprintf(&b, fmt, ap);
    va_end(ap);

    zb_free(err->message);
    err->message = NULL;
    if (rc == 0) {
        err->message = zb_buf_detach(&b);
    }
    zb_buf_free(&b);
    err->status = status;
    return status;
}

zb_status zb_error_nomem(zb_error *err)
{
    if (err != NULL) {
        zb_free(err->message);
        /* Do not allocate to report an allocation failure. */
        err->message = NULL;
        err->status = ZB_ERR_NOMEM;
    }
    return ZB_ERR_NOMEM;
}

const char *zb_error_message(const zb_error *err)
{
    if (err == NULL) {
        return "unknown error";
    }
    if (err->message != NULL && err->message[0] != '\0') {
        return err->message;
    }
    switch (err->status) {
    case ZB_OK:
        return "no error";
    case ZB_ERR_USAGE:
        return "invalid usage";
    case ZB_ERR_IO:
        return "file error";
    case ZB_ERR_NET:
        return "network error";
    case ZB_ERR_HTTP:
        return "the server rejected the request";
    case ZB_ERR_PROTO:
        return "unexpected response from the server";
    case ZB_ERR_NOMEM:
        return "out of memory";
    case ZB_ERR_CANCELED:
        return "canceled";
    }
    return "unknown error";
}

const char *zb_status_name(zb_status status)
{
    switch (status) {
    case ZB_OK:
        return "ok";
    case ZB_ERR_USAGE:
        return "usage";
    case ZB_ERR_IO:
        return "io";
    case ZB_ERR_NET:
        return "network";
    case ZB_ERR_HTTP:
        return "http";
    case ZB_ERR_PROTO:
        return "protocol";
    case ZB_ERR_NOMEM:
        return "nomem";
    case ZB_ERR_CANCELED:
        return "canceled";
    }
    return "unknown";
}

/* One table, so no libcurl code reaches the user as raw curl_easy_strerror()
 * text (§9). Anything not listed falls through to a generic sentence that
 * still names what libcurl called it. */
const char *zb_curl_message(int curl_code)
{
    switch ((CURLcode)curl_code) {
    case CURLE_OK:
        return NULL;
    case CURLE_UNSUPPORTED_PROTOCOL:
        return "the API URL uses a protocol this build of libcurl cannot speak";
    case CURLE_URL_MALFORMAT:
        return "the API URL is malformed";
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "could not resolve the configured proxy";
    case CURLE_COULDNT_RESOLVE_HOST:
        return "could not resolve the server's hostname — check your DNS or "
               "your connection";
    case CURLE_COULDNT_CONNECT:
        return "could not connect to the server — it may be down, or you may "
               "be offline";
    case CURLE_OPERATION_TIMEDOUT:
        return "the connection timed out";
    case CURLE_SSL_CONNECT_ERROR:
        return "the TLS handshake failed";
    case CURLE_PEER_FAILED_VERIFICATION:
        return "the server's TLS certificate could not be verified";
    case CURLE_SSL_CACERT_BADFILE:
        return "the CA certificate bundle could not be read";
    case CURLE_GOT_NOTHING:
        return "the server closed the connection without responding";
    case CURLE_SEND_ERROR:
        return "the connection dropped while sending data";
    case CURLE_RECV_ERROR:
        return "the connection dropped while receiving data";
    case CURLE_PARTIAL_FILE:
        return "the transfer ended early — fewer bytes arrived than the server "
               "promised";
    case CURLE_WRITE_ERROR:
        return "could not write the downloaded data to disk";
    case CURLE_READ_ERROR:
        return "could not read from the source file";
    case CURLE_ABORTED_BY_CALLBACK:
        return "canceled";
    case CURLE_OUT_OF_MEMORY:
        return "out of memory";
    case CURLE_TOO_MANY_REDIRECTS:
        return "the server redirected too many times";
    case CURLE_RANGE_ERROR:
        return "the server does not support resuming this download";
    default:
        break;
    }
    return "the transfer failed";
}
