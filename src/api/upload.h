/* upload.h — the chunked upload engine.
 *
 * One multi handle, one event loop, no threads. Each file is a small state
 * machine (init -> chunk* -> complete) and the engine runs up to `concurrency`
 * of them at once; chunks inside one file stay sequential, which is what makes
 * resume simple and the server's byte accounting predictable.
 *
 * Nothing is ever read into memory whole: chunk bodies are streamed straight
 * out of the source FILE* by a read callback, so peak RSS is flat whether the
 * file is 4 KB or 25 GB.
 */
#ifndef ZB_API_UPLOAD_H
#define ZB_API_UPLOAD_H

#include <stdint.h>

#include "api/client.h"
#include "store/sessions.h"
#include "util/error.h"
#include "util/json.h"

typedef struct zb_upload_engine zb_upload_engine;

typedef enum {
    ZB_RESUME_ASK = 0, /* prompt when an unfinished session exists */
    ZB_RESUME_ALWAYS,  /* --resume, or the config default */
    ZB_RESUME_NEVER    /* --no-resume: start over */
} zb_resume_mode;

/* Called once per file the moment its `complete` succeeds, before the engine
 * moves on. This is what makes the shelf write-through immediate: a crash a
 * second later must not lose the delete token. */
typedef void (*zb_upload_on_complete)(void *ctx, size_t job_index,
                                      const zb_json *result);

/* `collection_token` may be NULL. `base_dir` is where sessions.json lives.
 * Both `client` and `opt` are borrowed and must outlive the engine. */
zb_upload_engine *zb_upload_engine_new(zb_client *client, const zb_options *opt,
                                       const char *base_dir,
                                       const char *collection_token,
                                       int concurrency, zb_resume_mode resume,
                                       zb_error *err);

void zb_upload_engine_free(zb_upload_engine *engine);

/* Queue a file. The engine opens it immediately and keeps the handle, so a
 * file that is unreadable or that vanishes fails here rather than after
 * burning one of the 20 upload slots the rate limiter allows per 15 minutes.
 *
 * `delete_source_when_done` is for the temp file behind `upload -`. */
zb_status zb_upload_engine_add(zb_upload_engine *engine, const char *path,
                               const char *filename, const char *mime_type,
                               int delete_source_when_done, zb_error *err);

void zb_upload_engine_set_callback(zb_upload_engine *engine,
                                   zb_upload_on_complete cb, void *ctx);

/* Asked once per file that has an unfinished session, before anything is sent.
 * Return 1 to resume, 0 to start over. Deciding up front keeps the question
 * away from a screen that already has a progress bar on it. */
typedef int (*zb_upload_confirm_resume)(void *ctx, const char *path,
                                        uint64_t bytes_done, uint64_t total);

/* Settle the resume question for every queued file. Under ZB_RESUME_ALWAYS or
 * ZB_RESUME_NEVER the callback is never called and may be NULL. Call this
 * before zb_upload_engine_run(); if you skip it, nothing resumes. */
void zb_upload_engine_resolve_resume(zb_upload_engine *engine,
                                     zb_upload_confirm_resume confirm,
                                     void *ctx);

/* Run every queued file to completion or failure.
 *
 * Returns ZB_OK if every file succeeded, ZB_ERR_CANCELED if interrupted, and
 * otherwise the status of the first failure — per-file errors are readable
 * through zb_upload_job_error(). */
zb_status zb_upload_engine_run(zb_upload_engine *engine, zb_error *err);

size_t zb_upload_count(const zb_upload_engine *engine);
const char *zb_upload_job_filename(const zb_upload_engine *engine, size_t i);
const char *zb_upload_job_path(const zb_upload_engine *engine, size_t i);
uint64_t zb_upload_job_size(const zb_upload_engine *engine, size_t i);
zb_status zb_upload_job_status(const zb_upload_engine *engine, size_t i);
const zb_error *zb_upload_job_error(const zb_upload_engine *engine, size_t i);
const zb_json *zb_upload_job_result(const zb_upload_engine *engine, size_t i);

#endif /* ZB_API_UPLOAD_H */
