/* sessions.h — in-flight uploads, so an interrupted 20 GB transfer can be
 * finished rather than restarted (§8).
 *
 * Written after every chunk with the same atomic 0600 discipline as the shelf.
 * Rewriting the whole file every 20 MiB is cheap; a partial write is not.
 */
#ifndef ZB_STORE_SESSIONS_H
#define ZB_STORE_SESSIONS_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

#define ZB_SESSIONS_VERSION 1

typedef struct {
    char *token;
    char *delete_token;
    char *source_path;
    char *filename;
    char *mime_type;
    char *collection_token; /* NULL for a standalone upload */
    uint64_t size;
    int64_t mtime; /* of the source file, to detect it changing under us */
    uint64_t chunk_size;
    uint64_t total_chunks;
    uint8_t *sent;   /* bitmap, (total_chunks + 7) / 8 bytes */
    size_t sent_len; /* bytes in `sent` */
    int64_t updated_at;
} zb_session;

typedef struct {
    zb_session *items;
    size_t count;
    size_t capacity;
} zb_sessions;

void zb_sessions_init(zb_sessions *s);
void zb_sessions_free(zb_sessions *s);

/* Missing file means no sessions. A malformed file is reported rather than
 * overwritten — it may hold a delete token for an upload that exists. */
zb_status zb_sessions_load(zb_sessions *s, const char *base_dir, zb_error *err);
zb_status zb_sessions_save(const zb_sessions *s, const char *base_dir,
                           zb_error *err);

/* Match on source path + size + mtime, so a file edited since the interrupted
 * run is never resumed into the old session. NULL when there is no match. */
zb_session *zb_sessions_find_source(zb_sessions *s, const char *source_path,
                                    uint64_t size, int64_t mtime);
zb_session *zb_sessions_find_token(zb_sessions *s, const char *token);

/* Take ownership of `session`'s heap fields; on success the caller must not
 * free them. On failure nothing is taken. */
zb_status zb_sessions_put(zb_sessions *s, zb_session *session, zb_error *err);

int zb_sessions_remove(zb_sessions *s, const char *token);

/* Drop anything older than the server's idle session TTL — those tokens are
 * already gone server-side and would never resume. Returns how many went. */
size_t zb_sessions_prune_stale(zb_sessions *s, int64_t now);

void zb_session_free(zb_session *session);

/* Chunk bitmap. `index` is not range-checked by the getter: callers already
 * know total_chunks. */
int zb_session_alloc_bitmap(zb_session *session, uint64_t total_chunks);
void zb_session_mark(zb_session *session, uint64_t index);
int zb_session_has(const zb_session *session, uint64_t index);
uint64_t zb_session_count_sent(const zb_session *session);

#endif /* ZB_STORE_SESSIONS_H */
