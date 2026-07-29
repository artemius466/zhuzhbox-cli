#include "api/upload.h"

#include <stdio.h>
#include <string.h>

#include "format/progress.h"
#include "zb_limits.h"
#include "store/paths.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/str.h"

/* How many times we will re-ask the server which chunks it has after a 409 on
 * complete. Two rounds is generous; more than that means something is wrong
 * that retrying will not fix. */
#define ZB_MAX_RECONCILE_ROUNDS 3

typedef enum {
    JOB_START = 0,     /* decide between resume, single-shot and init */
    JOB_INIT,          /* POST /v1/upload/init in flight */
    JOB_INIT_BACKOFF,  /* 503 with a Retry-After we are waiting out */
    JOB_STATUS,        /* GET /v1/upload/{token}/status in flight */
    JOB_CHUNK,         /* PUT one chunk in flight */
    JOB_CHUNK_BACKOFF, /* between chunk retries */
    JOB_CONTROL_BACKOFF, /* between retries of a repeatable control request */
    JOB_COMPLETE,      /* POST /v1/upload/{token}/complete in flight */
    JOB_VERIFY,        /* GET /v1/upload/{token} after a lost complete reply */
    JOB_SHAREX,        /* POST /v1/sharex in flight */
    JOB_DONE,
    JOB_FAILED
} job_state;

typedef struct {
    /* Input. */
    char *path;
    char *filename;
    char *mime_type;
    uint64_t size;
    int64_t mtime;
    int delete_source;

    /* Upload state. */
    job_state state;
    FILE *fp;
    char *token;
    char *delete_token;
    uint64_t chunk_size;
    uint64_t total_chunks;
    uint8_t *sent;
    size_t sent_len;
    uint64_t chunk_index;     /* the chunk currently in flight */
    uint64_t chunk_offset;    /* its byte offset in the file */
    uint64_t chunk_length;    /* its exact length */
    uint64_t chunk_remaining; /* still to hand to libcurl */
    unsigned attempt;
    unsigned init_retries;
    unsigned reconcile_rounds;
    unsigned complete_attempts;
    job_state retry_state; /* which request JOB_CONTROL_BACKOFF will re-issue */
    uint64_t wake_at_ms;
    int used_single_shot;
    int resumed;
    int resume_allowed; /* settled by zb_upload_engine_resolve_resume */

    /* Transfer plumbing, valid only while a request is in flight. */
    CURL *easy;
    zb_xfer xfer;
    struct curl_slist *headers;
    char *url;

    /* Progress accounting. */
    uint64_t confirmed_bytes; /* chunks the server has acknowledged */
    uint64_t live_bytes;      /* confirmed + the in-flight chunk's progress */

    /* Output. */
    zb_json *result;
    zb_error error;
    zb_status status;
} zb_job;

struct zb_upload_engine {
    zb_client *client;
    const zb_options *opt;
    char *base_dir;
    char *collection_token;
    int concurrency;
    zb_resume_mode resume;

    zb_job *jobs;
    size_t job_count;
    size_t job_capacity;
    size_t in_flight;

    zb_sessions sessions;
    int sessions_dirty;

    zb_progress progress;
    uint64_t total_bytes;

    zb_upload_on_complete on_complete;
    void *callback_ctx;
};

/* ------------------------------------------------------------------ */
/* Chunk bitmap                                                         */
/* ------------------------------------------------------------------ */

static int job_alloc_bitmap(zb_job *job, uint64_t total_chunks)
{
    size_t bytes;

    if (total_chunks == 0 || total_chunks > (UINT64_C(1) << 32)) {
        return -1;
    }
    bytes = (size_t)((total_chunks + 7) / 8);
    zb_free(job->sent);
    job->sent = zb_calloc(bytes, 1);
    if (job->sent == NULL) {
        job->sent_len = 0;
        return -1;
    }
    job->sent_len = bytes;
    job->total_chunks = total_chunks;
    return 0;
}

static void job_mark(zb_job *job, uint64_t index)
{
    size_t byte = (size_t)(index / 8);
    if (job->sent != NULL && byte < job->sent_len) {
        job->sent[byte] |= (uint8_t)(1u << (index % 8));
    }
}

static int job_has(const zb_job *job, uint64_t index)
{
    size_t byte = (size_t)(index / 8);
    if (job->sent == NULL || byte >= job->sent_len) {
        return 0;
    }
    return (job->sent[byte] & (uint8_t)(1u << (index % 8))) != 0;
}

/* Index of the first chunk the server does not have, or total_chunks when
 * everything is accounted for. */
static uint64_t job_next_missing(const zb_job *job)
{
    uint64_t i;
    for (i = 0; i < job->total_chunks; i++) {
        if (!job_has(job, i)) {
            return i;
        }
    }
    return job->total_chunks;
}

static uint64_t job_chunk_length(const zb_job *job, uint64_t index)
{
    uint64_t offset = index * job->chunk_size;
    uint64_t remaining;

    if (offset >= job->size) {
        return 0;
    }
    remaining = job->size - offset;
    return remaining < job->chunk_size ? remaining : job->chunk_size;
}

static void job_recount_confirmed(zb_job *job)
{
    uint64_t i;
    uint64_t total = 0;
    for (i = 0; i < job->total_chunks; i++) {
        if (job_has(job, i)) {
            total += job_chunk_length(job, i);
        }
    }
    job->confirmed_bytes = total;
    job->live_bytes = total;
}

/* The server tells us chunkSize and totalChunks separately from size; if they
 * do not agree we must not seek or allocate on the basis of them (§2.1). */
static int job_geometry_is_sane(const zb_job *job)
{
    uint64_t expected;

    if (job->chunk_size == 0 || job->total_chunks == 0) {
        return 0;
    }
    if (job->chunk_size > (UINT64_C(1) << 32)) {
        return 0;
    }
    if (job->total_chunks > UINT64_MAX / job->chunk_size) {
        return 0; /* the multiply below would wrap */
    }
    expected = job->total_chunks * job->chunk_size;
    if (expected < job->size) {
        return 0; /* not enough chunks to cover the file */
    }
    if (job->size > 0 && (job->total_chunks - 1) * job->chunk_size >= job->size) {
        return 0; /* one chunk too many: the last one would be empty */
    }
    if (job->size == 0 && job->total_chunks != 1) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Job lifecycle                                                        */
/* ------------------------------------------------------------------ */

static void job_release_transfer(zb_upload_engine *engine, zb_job *job)
{
    if (job->easy != NULL) {
        curl_multi_remove_handle(zb_client_multi(engine->client), job->easy);
        curl_easy_cleanup(job->easy);
        job->easy = NULL;
        if (engine->in_flight > 0) {
            engine->in_flight--;
        }
    }
    if (job->headers != NULL) {
        curl_slist_free_all(job->headers);
        job->headers = NULL;
    }
    zb_free(job->url);
    job->url = NULL;
    zb_xfer_reset(&job->xfer);
}

static void job_free(zb_upload_engine *engine, zb_job *job)
{
    job_release_transfer(engine, job);
    zb_xfer_free(&job->xfer);
    if (job->fp != NULL) {
        fclose(job->fp);
        job->fp = NULL;
    }
    if (job->delete_source && job->path != NULL) {
        /* The temp file behind `upload -` never outlives the process. */
        zb_remove(job->path);
    }
    zb_free(job->path);
    zb_free(job->filename);
    zb_free(job->mime_type);
    zb_free(job->token);
    zb_free_secret(job->delete_token);
    zb_free(job->sent);
    zb_json_free(job->result);
    zb_error_clear(&job->error);
    memset(job, 0, sizeof(*job));
}

static void job_fail(zb_job *job, zb_status status)
{
    job->state = JOB_FAILED;
    job->status = status != ZB_OK ? status : ZB_ERR_PROTO;
    if (job->error.status == ZB_OK) {
        job->error.status = job->status;
    }
}

/* ------------------------------------------------------------------ */
/* Session persistence                                                  */
/* ------------------------------------------------------------------ */

static void engine_persist_session(zb_upload_engine *engine, zb_job *job)
{
    zb_session session;
    zb_error err;
    uint64_t i;

    if (job->token == NULL || engine->base_dir == NULL) {
        return;
    }
    zb_error_init(&err);
    memset(&session, 0, sizeof(session));

    session.token = zb_strdup(job->token);
    session.delete_token = zb_strdup(job->delete_token);
    session.source_path = zb_strdup(job->path);
    session.filename = zb_strdup(job->filename);
    session.mime_type = zb_strdup(job->mime_type);
    session.collection_token = zb_strdup(engine->collection_token);
    session.size = job->size;
    session.mtime = job->mtime;
    session.chunk_size = job->chunk_size;
    session.updated_at = zb_now_unix();

    if (session.token == NULL ||
        zb_session_alloc_bitmap(&session, job->total_chunks) != 0) {
        zb_session_free(&session);
        zb_error_clear(&err);
        return;
    }
    for (i = 0; i < job->total_chunks; i++) {
        if (job_has(job, i)) {
            zb_session_mark(&session, i);
        }
    }

    if (zb_sessions_put(&engine->sessions, &session, &err) != ZB_OK) {
        zb_session_free(&session);
    }
    /* Persisting is best-effort: failing to write the session file must not
     * kill an upload that is otherwise going fine. The user is told only if
     * the upload later needs the session and it is not there. */
    (void)zb_sessions_save(&engine->sessions, engine->base_dir, &err);
    zb_error_clear(&err);
}

static void engine_drop_session(zb_upload_engine *engine, const char *token)
{
    zb_error err;
    if (token == NULL || engine->base_dir == NULL) {
        return;
    }
    if (!zb_sessions_remove(&engine->sessions, token)) {
        return;
    }
    zb_error_init(&err);
    (void)zb_sessions_save(&engine->sessions, engine->base_dir, &err);
    zb_error_clear(&err);
}

/* ------------------------------------------------------------------ */
/* Request construction                                                 */
/* ------------------------------------------------------------------ */

static void on_job_progress(void *ctx, uint64_t now, uint64_t total)
{
    zb_job *job = ctx;
    (void)total;
    job->live_bytes = now;
}

static zb_status job_begin_request(zb_upload_engine *engine, zb_job *job,
                                   const char *method, const char *path,
                                   const zb_json *body)
{
    CURLM *multi = zb_client_multi(engine->client);

    job_release_transfer(engine, job);

    job->url = zb_client_url(engine->client, path);
    if (job->url == NULL) {
        return zb_error_nomem(&job->error);
    }
    job->easy = zb_client_easy(engine->client, &job->xfer, &job->error);
    if (job->easy == NULL) {
        return job->error.status;
    }
    curl_easy_setopt(job->easy, CURLOPT_URL, job->url);

    if (body != NULL) {
        if (zb_client_set_json_body(job->easy, body, &job->headers,
                                    &job->error) != ZB_OK) {
            return job->error.status;
        }
    }
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(job->easy, CURLOPT_POST, 1L);
        if (body == NULL) {
            curl_easy_setopt(job->easy, CURLOPT_POSTFIELDSIZE, 0L);
            curl_easy_setopt(job->easy, CURLOPT_COPYPOSTFIELDS, "");
        }
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(job->easy, CURLOPT_CUSTOMREQUEST, method);
    }
    if (job->headers != NULL) {
        curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
    }
    if (curl_multi_add_handle(multi, job->easy) != CURLM_OK) {
        return zb_error_setf(&job->error, ZB_ERR_NET,
                             "could not schedule the request");
    }
    engine->in_flight++;
    return ZB_OK;
}

/* Streams the current chunk out of the source file. Never buffers it. */
static size_t chunk_read_cb(char *buffer, size_t size, size_t nitems,
                            void *userdata)
{
    zb_job *job = userdata;
    size_t want;
    size_t got;

    if (size != 0 && nitems > (size_t)-1 / size) {
        return CURL_READFUNC_ABORT;
    }
    want = size * nitems;
    if ((uint64_t)want > job->chunk_remaining) {
        want = (size_t)job->chunk_remaining;
    }
    if (want == 0) {
        return 0;
    }
    got = fread(buffer, 1, want, job->fp);
    if (got == 0 && ferror(job->fp)) {
        return CURL_READFUNC_ABORT;
    }
    job->chunk_remaining -= got;
    return got;
}

/* libcurl asks for this on a redirect or an auth retry. Without it, a retried
 * chunk would resume from wherever the file position happened to be, which
 * silently corrupts the upload (§3.1). */
static int chunk_seek_cb(void *userdata, curl_off_t offset, int origin)
{
    zb_job *job = userdata;
    int64_t target;

    if (origin != SEEK_SET) {
        return CURL_SEEKFUNC_CANTSEEK;
    }
    if (offset < 0 || (uint64_t)offset > job->chunk_length) {
        return CURL_SEEKFUNC_FAIL;
    }
    target = (int64_t)(job->chunk_offset + (uint64_t)offset);
    if (zb_fseek64(job->fp, target, SEEK_SET) != 0) {
        return CURL_SEEKFUNC_FAIL;
    }
    job->chunk_remaining = job->chunk_length - (uint64_t)offset;
    return CURL_SEEKFUNC_OK;
}

static zb_status job_start_chunk(zb_upload_engine *engine, zb_job *job,
                                 uint64_t index)
{
    char *path;
    zb_status rc;

    job->chunk_index = index;
    job->chunk_offset = index * job->chunk_size;
    job->chunk_length = job_chunk_length(job, index);
    job->chunk_remaining = job->chunk_length;

    /* Always re-seek before a chunk, including on a retry: a stale file
     * position is the one failure that would corrupt the upload silently. */
    if (zb_fseek64(job->fp, (int64_t)job->chunk_offset, SEEK_SET) != 0) {
        return zb_error_setf(&job->error, ZB_ERR_IO,
                             "cannot seek to offset %llu in %s",
                             (unsigned long long)job->chunk_offset, job->path);
    }

    path = zb_asprintf("/v1/upload/%s/chunk/%llu", job->token,
                       (unsigned long long)index);
    if (path == NULL) {
        return zb_error_nomem(&job->error);
    }

    job->xfer.on_progress = on_job_progress;
    job->xfer.progress_ctx = job;
    job->xfer.uploading = 1;
    job->xfer.base_offset = job->confirmed_bytes;

    rc = job_begin_request(engine, job, "PUT", path, NULL);
    zb_free(path);
    if (rc != ZB_OK) {
        return rc;
    }

    curl_easy_setopt(job->easy, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(job->easy, CURLOPT_READFUNCTION, chunk_read_cb);
    curl_easy_setopt(job->easy, CURLOPT_READDATA, job);
    curl_easy_setopt(job->easy, CURLOPT_SEEKFUNCTION, chunk_seek_cb);
    curl_easy_setopt(job->easy, CURLOPT_SEEKDATA, job);
    /* An exact Content-Length: the server rejects a chunk whose length is not
     * what it expects, and chunked transfer encoding has no length at all. */
    curl_easy_setopt(job->easy, CURLOPT_INFILESIZE_LARGE,
                     (curl_off_t)job->chunk_length);
    {
        struct curl_slist *grown = curl_slist_append(
            job->headers, "Content-Type: application/octet-stream");
        if (grown == NULL) {
            return zb_error_nomem(&job->error);
        }
        job->headers = grown;
        /* Some proxies add this; libcurl would otherwise wait 1s for a 100. */
        grown = curl_slist_append(job->headers, "Expect:");
        if (grown != NULL) {
            job->headers = grown;
        }
        curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);
    }
    job->state = JOB_CHUNK;
    return ZB_OK;
}

static zb_status job_start_init(zb_upload_engine *engine, zb_job *job)
{
    zb_json *body = zb_json_new_object();
    zb_status rc;

    if (body == NULL) {
        return zb_error_nomem(&job->error);
    }
    if (zb_json_obj_set_str(body, "filename", job->filename) != 0 ||
        zb_json_obj_set_u64(body, "size", job->size) != 0 ||
        zb_json_obj_set_str(body, "mimeType", job->mime_type) != 0) {
        zb_json_free(body);
        return zb_error_nomem(&job->error);
    }
    if (engine->collection_token != NULL &&
        zb_json_obj_set_str(body, "collectionToken", engine->collection_token) !=
            0) {
        zb_json_free(body);
        return zb_error_nomem(&job->error);
    }

    job->xfer.on_progress = NULL;
    rc = job_begin_request(engine, job, "POST", "/v1/upload/init", body);
    zb_json_free(body);
    if (rc == ZB_OK) {
        job->state = JOB_INIT;
    }
    return rc;
}

static zb_status job_start_status(zb_upload_engine *engine, zb_job *job)
{
    char *path = zb_asprintf("/v1/upload/%s/status", job->token);
    zb_status rc;

    if (path == NULL) {
        return zb_error_nomem(&job->error);
    }
    job->xfer.on_progress = NULL;
    rc = job_begin_request(engine, job, "GET", path, NULL);
    zb_free(path);
    if (rc == ZB_OK) {
        job->state = JOB_STATUS;
    }
    return rc;
}

static zb_status job_start_complete(zb_upload_engine *engine, zb_job *job)
{
    char *path = zb_asprintf("/v1/upload/%s/complete", job->token);
    zb_status rc;

    if (path == NULL) {
        return zb_error_nomem(&job->error);
    }
    job->xfer.on_progress = NULL;
    rc = job_begin_request(engine, job, "POST", path, NULL);
    zb_free(path);
    if (rc == ZB_OK) {
        job->state = JOB_COMPLETE;
        job->complete_attempts++;
    }
    return rc;
}

/* If a `complete` reply was lost to a dropped connection, the upload may well
 * have succeeded and the retry will get a 404 because the session is already
 * gone. Ask for the file's metadata before declaring the session expired. */
static zb_status job_start_verify(zb_upload_engine *engine, zb_job *job)
{
    char *path = zb_asprintf("/v1/upload/%s", job->token);
    zb_status rc;

    if (path == NULL) {
        return zb_error_nomem(&job->error);
    }
    job->xfer.on_progress = NULL;
    rc = job_begin_request(engine, job, "GET", path, NULL);
    zb_free(path);
    if (rc == ZB_OK) {
        job->state = JOB_VERIFY;
    }
    return rc;
}

/* The one-request path for small files: identical result, one round trip
 * instead of three. Still streamed from the FILE*. */
static zb_status job_start_single_shot(zb_upload_engine *engine, zb_job *job)
{
    zb_status rc;
    char *header;

    if (zb_fseek64(job->fp, 0, SEEK_SET) != 0) {
        return zb_error_setf(&job->error, ZB_ERR_IO, "cannot rewind %s",
                             job->path);
    }
    job->chunk_offset = 0;
    job->chunk_length = job->size;
    job->chunk_remaining = job->size;

    job->xfer.on_progress = on_job_progress;
    job->xfer.progress_ctx = job;
    job->xfer.uploading = 1;
    job->xfer.base_offset = 0;

    /* Headers must be built after job_begin_request, which resets the handle
     * and frees any list already attached to the job. */
    rc = job_begin_request(engine, job, "POST", "/v1/sharex", NULL);
    if (rc != ZB_OK) {
        return rc;
    }

    header = zb_asprintf("X-Filename: %s", job->filename);
    if (header == NULL) {
        return zb_error_nomem(&job->error);
    }
    {
        struct curl_slist *grown = curl_slist_append(job->headers, header);
        zb_free(header);
        if (grown == NULL) {
            return zb_error_nomem(&job->error);
        }
        job->headers = grown;
        header = zb_asprintf("Content-Type: %s", job->mime_type);
        if (header == NULL) {
            return zb_error_nomem(&job->error);
        }
        grown = curl_slist_append(job->headers, header);
        zb_free(header);
        if (grown == NULL) {
            return zb_error_nomem(&job->error);
        }
        job->headers = grown;
        /* Without this libcurl waits a second for a 100-continue that some
         * proxies never send. */
        grown = curl_slist_append(job->headers, "Expect:");
        if (grown != NULL) {
            job->headers = grown;
        }
    }

    curl_easy_setopt(job->easy, CURLOPT_POST, 1L);
    curl_easy_setopt(job->easy, CURLOPT_READFUNCTION, chunk_read_cb);
    curl_easy_setopt(job->easy, CURLOPT_READDATA, job);
    curl_easy_setopt(job->easy, CURLOPT_SEEKFUNCTION, chunk_seek_cb);
    curl_easy_setopt(job->easy, CURLOPT_SEEKDATA, job);
    curl_easy_setopt(job->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)job->size);
    curl_easy_setopt(job->easy, CURLOPT_HTTPHEADER, job->headers);

    job->used_single_shot = 1;
    job->state = JOB_SHAREX;
    return ZB_OK;
}

/* ------------------------------------------------------------------ */
/* Completion handling                                                  */
/* ------------------------------------------------------------------ */

static void job_succeed(zb_upload_engine *engine, zb_job *job, zb_json *result,
                        size_t job_index)
{
    zb_json_free(job->result);
    job->result = result;
    job->state = JOB_DONE;
    job->status = ZB_OK;
    job->confirmed_bytes = job->size;
    job->live_bytes = job->size;

    engine_drop_session(engine, job->token);
    if (engine->on_complete != NULL) {
        engine->on_complete(engine->callback_ctx, job_index, result);
    }
}

static void handle_init_response(zb_upload_engine *engine, zb_job *job,
                                 zb_json *response)
{
    const char *token = NULL;
    const char *delete_token = NULL;
    uint64_t chunk_size = 0;
    uint64_t total_chunks = 0;

    if (!zb_json_get_str(response, "token", &token) ||
        !zb_json_get_u64(response, "chunkSize", &chunk_size) ||
        !zb_json_get_u64(response, "totalChunks", &total_chunks)) {
        zb_error_setf(&job->error, ZB_ERR_PROTO,
                      "the server's upload/init response was missing token, "
                      "chunkSize or totalChunks");
        job_fail(job, ZB_ERR_PROTO);
        return;
    }
    (void)zb_json_get_str(response, "deleteToken", &delete_token);

    zb_free(job->token);
    job->token = zb_strdup(token);
    if (delete_token != NULL) {
        zb_free_secret(job->delete_token);
        job->delete_token = zb_strdup(delete_token);
    }
    job->chunk_size = chunk_size;
    if (job->token == NULL || job_alloc_bitmap(job, total_chunks) != 0 ||
        !job_geometry_is_sane(job)) {
        zb_error_setf(&job->error, ZB_ERR_PROTO,
                      "the server described %s as %llu chunks of %llu bytes, "
                      "which does not add up to its %llu bytes",
                      job->filename, (unsigned long long)total_chunks,
                      (unsigned long long)chunk_size,
                      (unsigned long long)job->size);
        job_fail(job, ZB_ERR_PROTO);
        return;
    }
    job_recount_confirmed(job);
    engine_persist_session(engine, job);
}

static void handle_status_response(zb_upload_engine *engine, zb_job *job,
                                   zb_json *response)
{
    const zb_json *received;
    size_t n;
    size_t i;
    uint64_t chunk_size = 0;
    uint64_t total_chunks = 0;

    if (zb_json_get_u64(response, "chunkSize", &chunk_size) &&
        zb_json_get_u64(response, "totalChunks", &total_chunks)) {
        job->chunk_size = chunk_size;
        if (job_alloc_bitmap(job, total_chunks) != 0 ||
            !job_geometry_is_sane(job)) {
            zb_error_setf(&job->error, ZB_ERR_PROTO,
                          "the resumed session's chunk layout does not match "
                          "%s any more — start the upload again",
                          job->filename);
            job_fail(job, ZB_ERR_PROTO);
            return;
        }
    }

    /* The server is authoritative: whatever we thought we had sent, this is
     * what it actually holds (§8). */
    received = zb_json_get(response, "receivedChunks");
    n = zb_json_array_len(received);
    for (i = 0; i < n; i++) {
        uint64_t index = 0;
        if (zb_json_as_u64(zb_json_at(received, i), &index) &&
            index < job->total_chunks) {
            job_mark(job, index);
        }
    }
    job_recount_confirmed(job);
    engine_persist_session(engine, job);
}

/* Decide what a finished request means and move the job to its next state. */
static void job_advance(zb_upload_engine *engine, zb_job *job, size_t job_index,
                        CURLcode code)
{
    zb_json *response = NULL;
    long http_status = 0;
    zb_status rc;
    job_state finished_state = job->state;

    if (code != CURLE_OK) {
        zb_error err;
        zb_error_init(&err);
        zb_client_curl_error(code, job->url != NULL ? job->url : "", &err);

        if (err.status == ZB_ERR_CANCELED) {
            zb_error_move(&job->error, &err);
            job_fail(job, ZB_ERR_CANCELED);
            job_release_transfer(engine, job);
            return;
        }
        /* A network blip is normal on a multi-hour upload; retry quietly and
         * only surface the failure once the budget is gone.
         *
         * Only requests that are safe to repeat are retried. /init and
         * /sharex are not: repeating them would burn another of the 20 init
         * slots the rate limiter allows, or upload the whole file twice. */
        if ((finished_state == JOB_CHUNK || finished_state == JOB_STATUS ||
             finished_state == JOB_COMPLETE || finished_state == JOB_VERIFY) &&
            job->attempt < ZB_CHUNK_MAX_RETRIES) {
            job->attempt++;
            job->wake_at_ms = zb_now_ms() + ZB_CHUNK_BACKOFF_MS(job->attempt);
            job->state = finished_state == JOB_CHUNK ? JOB_CHUNK_BACKOFF
                                                     : JOB_CONTROL_BACKOFF;
            job->retry_state = finished_state;
            zb_error_clear(&err);
            job_release_transfer(engine, job);
            return;
        }
        if (job->token != NULL) {
            zb_error_setf(&job->error, err.status, "%s — run `zhuzhbox upload %s --resume` to continue",
                          zb_error_message(&err), job->path);
        } else {
            zb_error_move(&job->error, &err);
        }
        zb_error_clear(&err);
        job_fail(job, job->error.status);
        job_release_transfer(engine, job);
        return;
    }

    rc = zb_client_finish(engine->client, job->easy, &job->xfer,
                          job->url != NULL ? job->url : "", &response,
                          &http_status, &job->error);
    job_release_transfer(engine, job);

    if (rc != ZB_OK) {
        switch (finished_state) {
        case JOB_INIT:
        case JOB_SHAREX:
            /* 503 means the server is at capacity and told us when to come
             * back — in a header, not the body. */
            if (http_status == 503 && job->init_retries == 0) {
                long wait = job->error.retry_after_seconds;
                if (wait >= 0 && wait <= ZB_MAX_RETRY_AFTER_SECONDS) {
                    job->init_retries++;
                    job->wake_at_ms = zb_now_ms() + (uint64_t)wait * 1000u + 250u;
                    job->state = JOB_INIT_BACKOFF;
                    zb_progress_clear(&engine->progress);
                    zb_info(engine->opt,
                            "%s: the server is at upload capacity; retrying in "
                            "%ld second%s",
                            job->filename, wait, wait == 1 ? "" : "s");
                    zb_error_clear(&job->error);
                    zb_json_free(response);
                    return;
                }
                /* An hour-long Retry-After is not something to sleep through:
                 * say so and let the user decide when to come back (§9). */
                zb_error_setf(&job->error, ZB_ERR_HTTP,
                              "%s (the server asked us to wait %ld seconds, "
                              "which is too long to sit here for — try again "
                              "later)",
                              zb_error_message(&job->error), wait);
            }
            if (http_status == 413) {
                /* We check the cap before calling init, so reaching this is a
                 * bug on our side rather than a routine rejection (§9). */
                zb_error_setf(&job->error, ZB_ERR_HTTP,
                              "%s — the server rejected the size after we had "
                              "already accepted it locally, which should not "
                              "happen; please report this",
                              zb_error_message(&job->error));
            }
            break;

        case JOB_CHUNK:
            if (http_status == 404) {
                /* Idle sessions are dropped after 3 hours; retrying the same
                 * token will never work. */
                zb_error_setf(&job->error, ZB_ERR_HTTP,
                              "the upload session for %s has expired (the "
                              "server discards idle sessions after %d hours) — "
                              "run the upload again to start over",
                              job->filename,
                              ZB_UPLOAD_SESSION_TTL_MINUTES / 60);
                engine_drop_session(engine, job->token);
                break;
            }
            if (http_status >= 500 && job->attempt < ZB_CHUNK_MAX_RETRIES) {
                job->attempt++;
                job->wake_at_ms = zb_now_ms() + ZB_CHUNK_BACKOFF_MS(job->attempt);
                job->state = JOB_CHUNK_BACKOFF;
                zb_error_clear(&job->error);
                zb_json_free(response);
                return;
            }
            break;

        case JOB_COMPLETE:
            if (http_status == 409 &&
                job->reconcile_rounds < ZB_MAX_RECONCILE_ROUNDS) {
                /* Chunks are still missing and the server knows which. Ask,
                 * resend the gap, and try again rather than just failing. */
                job->reconcile_rounds++;
                zb_error_clear(&job->error);
                zb_json_free(response);
                if (job_start_status(engine, job) != ZB_OK) {
                    job_fail(job, job->error.status);
                }
                return;
            }
            if (http_status == 404) {
                /* If an earlier attempt's reply was lost, the upload may
                 * already have completed and taken the session with it. */
                if (job->complete_attempts > 1) {
                    zb_error_clear(&job->error);
                    zb_json_free(response);
                    if (job_start_verify(engine, job) != ZB_OK) {
                        job_fail(job, job->error.status);
                    }
                    return;
                }
                zb_error_setf(&job->error, ZB_ERR_HTTP,
                              "the upload session for %s has expired — run the "
                              "upload again to start over",
                              job->filename);
                engine_drop_session(engine, job->token);
            }
            break;

        case JOB_VERIFY:
            zb_error_setf(&job->error, ZB_ERR_HTTP,
                          "%s did not finish uploading and its session is gone "
                          "— run the upload again to start over",
                          job->filename);
            engine_drop_session(engine, job->token);
            break;

        case JOB_STATUS:
            if (http_status == 404) {
                zb_error_setf(&job->error, ZB_ERR_HTTP,
                              "the upload session for %s is gone — run the "
                              "upload again to start over",
                              job->filename);
                engine_drop_session(engine, job->token);
            }
            break;

        default:
            break;
        }
        zb_json_free(response);
        job_fail(job, rc);
        return;
    }

    /* --- success paths --- */
    switch (finished_state) {
    case JOB_SHAREX:
        job_succeed(engine, job, response, job_index);
        return;

    case JOB_INIT:
        handle_init_response(engine, job, response);
        zb_json_free(response);
        if (job->state == JOB_FAILED) {
            return;
        }
        break;

    case JOB_STATUS:
        handle_status_response(engine, job, response);
        zb_json_free(response);
        if (job->state == JOB_FAILED) {
            return;
        }
        break;

    case JOB_CHUNK:
        job_mark(job, job->chunk_index);
        job->attempt = 0;
        job_recount_confirmed(job);
        engine_persist_session(engine, job);
        zb_json_free(response);
        break;

    case JOB_COMPLETE:
        job_succeed(engine, job, response, job_index);
        return;

    case JOB_VERIFY:
        /* The upload did land; a reply was simply lost on the way back. The
         * metadata record has no url or deleteToken, so fill in what we know
         * from init rather than reporting a half-empty result. */
        if (response != NULL) {
            const char *existing = NULL;
            if (!zb_json_get_str(response, "deleteToken", &existing) &&
                job->delete_token != NULL) {
                (void)zb_json_obj_set_str(response, "deleteToken",
                                          job->delete_token);
            }
            if (!zb_json_get_str(response, "url", &existing)) {
                char *url = zb_asprintf("%s/d/%s",
                                        zb_opt_download_host(engine->opt),
                                        job->token);
                if (url != NULL) {
                    (void)zb_json_obj_set_str(response, "url", url);
                    zb_free(url);
                }
            }
        }
        job_succeed(engine, job, response, job_index);
        return;

    default:
        zb_json_free(response);
        break;
    }

    /* Pick the next request: the next gap, or completion. */
    {
        uint64_t next = job_next_missing(job);
        if (next >= job->total_chunks) {
            if (job_start_complete(engine, job) != ZB_OK) {
                job_fail(job, job->error.status);
            }
        } else if (job_start_chunk(engine, job, next) != ZB_OK) {
            job_fail(job, job->error.status);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Engine                                                               */
/* ------------------------------------------------------------------ */

zb_upload_engine *zb_upload_engine_new(zb_client *client, const zb_options *opt,
                                       const char *base_dir,
                                       const char *collection_token,
                                       int concurrency, zb_resume_mode resume,
                                       zb_error *err)
{
    zb_upload_engine *engine = zb_calloc(1, sizeof(*engine));

    if (engine == NULL) {
        zb_error_nomem(err);
        return NULL;
    }
    engine->client = client;
    engine->opt = opt;
    engine->resume = resume;
    engine->concurrency = concurrency < 1 ? 1
                          : (concurrency > ZB_MAX_CONCURRENCY
                                 ? ZB_MAX_CONCURRENCY
                                 : concurrency);
    zb_sessions_init(&engine->sessions);

    if (base_dir != NULL) {
        engine->base_dir = zb_strdup(base_dir);
        if (engine->base_dir == NULL) {
            zb_upload_engine_free(engine);
            zb_error_nomem(err);
            return NULL;
        }
        /* A malformed sessions file must not block an upload that does not
         * need to resume; report nothing and carry on with none loaded. */
        {
            zb_error load_err;
            zb_error_init(&load_err);
            if (zb_sessions_load(&engine->sessions, base_dir, &load_err) !=
                ZB_OK) {
                zb_sessions_init(&engine->sessions);
            }
            zb_error_clear(&load_err);
        }
        if (zb_sessions_prune_stale(&engine->sessions, zb_now_unix()) > 0) {
            zb_error prune_err;
            zb_error_init(&prune_err);
            (void)zb_sessions_save(&engine->sessions, base_dir, &prune_err);
            zb_error_clear(&prune_err);
        }
    }

    if (collection_token != NULL) {
        engine->collection_token = zb_strdup(collection_token);
        if (engine->collection_token == NULL) {
            zb_upload_engine_free(engine);
            zb_error_nomem(err);
            return NULL;
        }
    }
    return engine;
}

void zb_upload_engine_free(zb_upload_engine *engine)
{
    size_t i;
    if (engine == NULL) {
        return;
    }
    for (i = 0; i < engine->job_count; i++) {
        job_free(engine, &engine->jobs[i]);
    }
    zb_free(engine->jobs);
    zb_sessions_free(&engine->sessions);
    zb_progress_free(&engine->progress);
    zb_free(engine->base_dir);
    zb_free(engine->collection_token);
    zb_free(engine);
}

void zb_upload_engine_set_callback(zb_upload_engine *engine,
                                   zb_upload_on_complete cb, void *ctx)
{
    engine->on_complete = cb;
    engine->callback_ctx = ctx;
}

zb_status zb_upload_engine_add(zb_upload_engine *engine, const char *path,
                               const char *filename, const char *mime_type,
                               int delete_source_when_done, zb_error *err)
{
    zb_job job;
    zb_stat_info info;

    memset(&job, 0, sizeof(job));
    zb_xfer_init(&job.xfer);
    zb_error_init(&job.error);

    if (zb_stat(path, &info) != 0) {
        return zb_error_setf(err, ZB_ERR_IO, "cannot read %s", path);
    }
    if (info.is_dir) {
        return zb_error_setf(err, ZB_ERR_USAGE,
                             "%s is a directory — pass files, not directories",
                             path);
    }
    /* Checked before any network call so a too-large file fails instantly
     * rather than after a round trip (§3.5). */
    if (info.size > ZB_MAX_UPLOAD_BYTES) {
        return zb_error_setf(
            err, ZB_ERR_USAGE,
            "%s is %llu bytes, over the %llu byte limit for a single file",
            path, (unsigned long long)info.size,
            (unsigned long long)ZB_MAX_UPLOAD_BYTES);
    }

    job.size = info.size;
    job.mtime = info.mtime;
    job.delete_source = delete_source_when_done;
    job.path = zb_strdup(path);
    job.filename = zb_strdup(filename);
    job.mime_type = zb_strdup(mime_type);
    if (job.path == NULL || job.filename == NULL || job.mime_type == NULL) {
        job_free(engine, &job);
        return zb_error_nomem(err);
    }

    /* Open now and hold the handle: a file that cannot be read should not
     * consume one of the 20 init slots the rate limiter allows. */
    job.fp = zb_fopen(path, "rb");
    if (job.fp == NULL) {
        job_free(engine, &job);
        return zb_error_setf(err, ZB_ERR_IO, "cannot open %s for reading", path);
    }

    if (engine->job_count == engine->job_capacity) {
        size_t cap = engine->job_capacity != 0 ? engine->job_capacity * 2 : 8;
        zb_job *grown;
        if (cap > (size_t)-1 / sizeof(*grown)) {
            job_free(engine, &job);
            return zb_error_nomem(err);
        }
        grown = zb_realloc(engine->jobs, cap * sizeof(*grown));
        if (grown == NULL) {
            job_free(engine, &job);
            return zb_error_nomem(err);
        }
        engine->jobs = grown;
        engine->job_capacity = cap;
    }
    engine->jobs[engine->job_count++] = job;
    engine->total_bytes += job.size;
    return ZB_OK;
}

/* Decide how a queued job starts: resume an existing session, take the
 * single-request path, or run a normal init. */
static void job_kick_off(zb_upload_engine *engine, zb_job *job)
{
    zb_session *session = NULL;

    if (job->resume_allowed) {
        session = zb_sessions_find_source(&engine->sessions, job->path,
                                          job->size, job->mtime);
    }
    if (session != NULL && session->token != NULL) {
        /* We trust the session only far enough to ask the server what it
         * actually has; /status is what decides (§8). */
        job->token = zb_strdup(session->token);
        job->delete_token = zb_strdup(session->delete_token);
        job->chunk_size = session->chunk_size;
        if (job->token != NULL &&
            job_alloc_bitmap(job, session->total_chunks) == 0) {
            uint64_t i;
            for (i = 0; i < session->total_chunks; i++) {
                if (zb_session_has(session, i)) {
                    job_mark(job, i);
                }
            }
            job->resumed = 1;
            job_recount_confirmed(job);
            if (job_start_status(engine, job) != ZB_OK) {
                job_fail(job, job->error.status);
            }
            return;
        }
        /* Could not rebuild the session; fall through to a fresh upload. */
        zb_free(job->token);
        job->token = NULL;
    }

    if (engine->opt->cfg.single_shot && engine->collection_token == NULL &&
        job->size > 0 && job->size <= engine->opt->cfg.single_shot_max) {
        if (job_start_single_shot(engine, job) != ZB_OK) {
            job_fail(job, job->error.status);
        }
        return;
    }
    if (job_start_init(engine, job) != ZB_OK) {
        job_fail(job, job->error.status);
    }
}

void zb_upload_engine_resolve_resume(zb_upload_engine *engine,
                                     zb_upload_confirm_resume confirm,
                                     void *ctx)
{
    size_t i;

    for (i = 0; i < engine->job_count; i++) {
        zb_job *job = &engine->jobs[i];
        const zb_session *session;

        job->resume_allowed = 0;
        if (engine->resume == ZB_RESUME_NEVER) {
            continue;
        }
        session = zb_sessions_find_source(&engine->sessions, job->path,
                                          job->size, job->mtime);
        if (session == NULL) {
            continue;
        }
        if (engine->resume == ZB_RESUME_ALWAYS) {
            job->resume_allowed = 1;
            continue;
        }
        if (confirm == NULL) {
            continue;
        }
        {
            uint64_t done = zb_session_count_sent(session) * session->chunk_size;
            if (done > job->size) {
                done = job->size;
            }
            job->resume_allowed = confirm(ctx, job->path, done, job->size) ? 1
                                                                          : 0;
        }
    }
}

static int job_is_active(const zb_job *job)
{
    return job->state != JOB_DONE && job->state != JOB_FAILED;
}

static int job_is_waiting(const zb_job *job)
{
    return job->state == JOB_START || job->state == JOB_INIT_BACKOFF ||
           job->state == JOB_CHUNK_BACKOFF ||
           job->state == JOB_CONTROL_BACKOFF;
}

static void engine_update_progress(zb_upload_engine *engine)
{
    uint64_t total = 0;
    size_t i;

    for (i = 0; i < engine->job_count; i++) {
        total += engine->jobs[i].live_bytes;
    }
    zb_progress_update(&engine->progress, total);
}

/* A single redrawn line while we are waiting out a Retry-After, so the program
 * does not look hung. Suppressed with progress, i.e. under --quiet and --json. */
static void engine_draw_countdown(zb_upload_engine *engine, uint64_t remain_ms)
{
    if (!engine->opt->progress) {
        return;
    }
    fprintf(stderr, "\r  waiting %llus for the server\033[K",
            (unsigned long long)((remain_ms + 999) / 1000));
    fflush(stderr);
}

/* Cancel everything in flight so the process can unwind cleanly. Sessions have
 * already been persisted after each chunk, so the upload stays resumable. */
static void engine_abort(zb_upload_engine *engine)
{
    size_t i;
    for (i = 0; i < engine->job_count; i++) {
        zb_job *job = &engine->jobs[i];
        if (job_is_active(job)) {
            job_release_transfer(engine, job);
            if (job->error.status == ZB_OK) {
                zb_error_setf(&job->error, ZB_ERR_CANCELED, "interrupted");
            }
            job->state = JOB_FAILED;
            job->status = ZB_ERR_CANCELED;
        }
    }
}

zb_status zb_upload_engine_run(zb_upload_engine *engine, zb_error *err)
{
    CURLM *multi = zb_client_multi(engine->client);
    zb_status result = ZB_OK;
    size_t i;
    int canceled = 0;

    if (engine->job_count == 0) {
        return ZB_OK;
    }

    {
        const char *label = engine->job_count == 1
                                ? engine->jobs[0].filename
                                : NULL;
        char count_label[64];
        if (label == NULL) {
            (void)zb_snprintf(count_label, sizeof(count_label), "%llu files",
                              (unsigned long long)engine->job_count);
            label = count_label;
        }
        zb_progress_init(&engine->progress, label, engine->total_bytes,
                         engine->opt->progress);
    }

    for (;;) {
        int running = 0;
        int active = 0;
        uint64_t now = zb_now_ms();
        uint64_t next_wake = 0;

        if (zb_interrupted) {
            engine_abort(engine);
            canceled = 1;
            break;
        }

        /* Start whatever is ready, up to the concurrency budget. */
        for (i = 0; i < engine->job_count; i++) {
            zb_job *job = &engine->jobs[i];
            if (!job_is_active(job)) {
                continue;
            }
            active = 1;
            if (job->easy != NULL) {
                continue;
            }
            if ((size_t)engine->concurrency <= engine->in_flight) {
                continue;
            }
            if (job->state == JOB_START) {
                job_kick_off(engine, job);
            } else if (job->state == JOB_INIT_BACKOFF && now >= job->wake_at_ms) {
                if (job_start_init(engine, job) != ZB_OK) {
                    job_fail(job, job->error.status);
                }
            } else if (job->state == JOB_CHUNK_BACKOFF &&
                       now >= job->wake_at_ms) {
                if (job_start_chunk(engine, job, job->chunk_index) != ZB_OK) {
                    job_fail(job, job->error.status);
                }
            } else if (job->state == JOB_CONTROL_BACKOFF &&
                       now >= job->wake_at_ms) {
                zb_status again;
                switch (job->retry_state) {
                case JOB_STATUS:
                    again = job_start_status(engine, job);
                    break;
                case JOB_VERIFY:
                    again = job_start_verify(engine, job);
                    break;
                default:
                    again = job_start_complete(engine, job);
                    break;
                }
                if (again != ZB_OK) {
                    job_fail(job, job->error.status);
                }
            } else if (job_is_waiting(job)) {
                if (next_wake == 0 || job->wake_at_ms < next_wake) {
                    next_wake = job->wake_at_ms;
                }
            }
        }

        if (!active) {
            break;
        }

        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            zb_error_setf(err, ZB_ERR_NET, "the transfer loop failed");
            engine_abort(engine);
            result = ZB_ERR_NET;
            break;
        }

        /* Drain completions before waiting again. */
        for (;;) {
            int queued = 0;
            CURLMsg *msg = curl_multi_info_read(multi, &queued);
            size_t index;
            int found = 0;

            if (msg == NULL) {
                break;
            }
            if (msg->msg != CURLMSG_DONE) {
                continue;
            }
            for (index = 0; index < engine->job_count; index++) {
                if (engine->jobs[index].easy == msg->easy_handle) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                continue;
            }
            job_advance(engine, &engine->jobs[index], index, msg->data.result);
        }

        engine_update_progress(engine);

        if (running > 0) {
            int numfds = 0;
            /* 100 ms keeps the progress line lively and bounds how long a
             * Ctrl+C takes to be noticed. */
            curl_multi_poll(multi, NULL, 0, 100, &numfds);
        } else if (engine->in_flight == 0) {
            uint64_t sleep_ms = 100;
            if (next_wake > 0) {
                uint64_t then = zb_now_ms();
                sleep_ms = next_wake > then ? next_wake - then : 0;
                engine_draw_countdown(engine, next_wake > then
                                                  ? next_wake - then
                                                  : 0);
                if (sleep_ms > 250) {
                    sleep_ms = 250; /* stay responsive to Ctrl+C */
                }
            }
            if (sleep_ms > 0) {
                zb_sleep_ms((unsigned)sleep_ms);
            }
        }
    }

    zb_progress_clear(&engine->progress);
    if (!canceled && result == ZB_OK) {
        zb_progress_finish(&engine->progress);
    }

    if (canceled) {
        return zb_error_setf(err, ZB_ERR_CANCELED, "interrupted");
    }
    if (result != ZB_OK) {
        return result;
    }
    for (i = 0; i < engine->job_count; i++) {
        if (engine->jobs[i].status != ZB_OK) {
            return engine->jobs[i].status;
        }
    }
    return ZB_OK;
}

size_t zb_upload_count(const zb_upload_engine *engine)
{
    return engine->job_count;
}

const char *zb_upload_job_filename(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? engine->jobs[i].filename : NULL;
}

const char *zb_upload_job_path(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? engine->jobs[i].path : NULL;
}

uint64_t zb_upload_job_size(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? engine->jobs[i].size : 0;
}

zb_status zb_upload_job_status(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? engine->jobs[i].status : ZB_ERR_USAGE;
}

const zb_error *zb_upload_job_error(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? &engine->jobs[i].error : NULL;
}

const zb_json *zb_upload_job_result(const zb_upload_engine *engine, size_t i)
{
    return i < engine->job_count ? engine->jobs[i].result : NULL;
}
