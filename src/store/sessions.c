#include "store/sessions.h"

#include <string.h>

#include "zb_limits.h"
#include "store/atomic.h"
#include "store/paths.h"
#include "util/buf.h"
#include "util/json.h"
#include "util/str.h"

void zb_sessions_init(zb_sessions *s)
{
    memset(s, 0, sizeof(*s));
}

void zb_session_free(zb_session *session)
{
    if (session == NULL) {
        return;
    }
    zb_free(session->token);
    zb_free_secret(session->delete_token);
    zb_free(session->source_path);
    zb_free(session->filename);
    zb_free(session->mime_type);
    zb_free(session->collection_token);
    zb_free(session->sent);
    memset(session, 0, sizeof(*session));
}

void zb_sessions_free(zb_sessions *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->count; i++) {
        zb_session_free(&s->items[i]);
    }
    zb_free(s->items);
    zb_sessions_init(s);
}

int zb_session_alloc_bitmap(zb_session *session, uint64_t total_chunks)
{
    size_t bytes;

    /* total_chunks comes from the server; refuse anything that would overflow
     * the byte count or imply an absurd allocation (§2.1). */
    if (total_chunks == 0 || total_chunks > UINT64_C(1) << 32) {
        return -1;
    }
    bytes = (size_t)((total_chunks + 7) / 8);
    zb_free(session->sent);
    session->sent = zb_calloc(bytes, 1);
    if (session->sent == NULL) {
        session->sent_len = 0;
        return -1;
    }
    session->sent_len = bytes;
    session->total_chunks = total_chunks;
    return 0;
}

void zb_session_mark(zb_session *session, uint64_t index)
{
    size_t byte = (size_t)(index / 8);
    if (session->sent == NULL || byte >= session->sent_len) {
        return;
    }
    session->sent[byte] |= (uint8_t)(1u << (index % 8));
}

int zb_session_has(const zb_session *session, uint64_t index)
{
    size_t byte = (size_t)(index / 8);
    if (session->sent == NULL || byte >= session->sent_len) {
        return 0;
    }
    return (session->sent[byte] & (uint8_t)(1u << (index % 8))) != 0;
}

uint64_t zb_session_count_sent(const zb_session *session)
{
    uint64_t n = 0;
    uint64_t i;
    for (i = 0; i < session->total_chunks; i++) {
        if (zb_session_has(session, i)) {
            n++;
        }
    }
    return n;
}

zb_session *zb_sessions_find_token(zb_sessions *s, const char *token)
{
    size_t i;
    if (s == NULL || token == NULL) {
        return NULL;
    }
    for (i = 0; i < s->count; i++) {
        if (s->items[i].token != NULL &&
            strcmp(s->items[i].token, token) == 0) {
            return &s->items[i];
        }
    }
    return NULL;
}

zb_session *zb_sessions_find_source(zb_sessions *s, const char *source_path,
                                    uint64_t size, int64_t mtime)
{
    size_t i;
    if (s == NULL || source_path == NULL) {
        return NULL;
    }
    for (i = 0; i < s->count; i++) {
        const zb_session *item = &s->items[i];
        if (item->source_path == NULL) {
            continue;
        }
        /* Size and mtime both have to match: resuming into a session for a
         * file that has since been edited would upload a mix of two files. */
        if (strcmp(item->source_path, source_path) == 0 && item->size == size &&
            item->mtime == mtime) {
            return &s->items[i];
        }
    }
    return NULL;
}

zb_status zb_sessions_put(zb_sessions *s, zb_session *session, zb_error *err)
{
    zb_session *existing;

    if (session->token == NULL) {
        return zb_error_setf(err, ZB_ERR_PROTO, "session has no token");
    }
    existing = zb_sessions_find_token(s, session->token);
    if (existing != NULL) {
        zb_session_free(existing);
        *existing = *session;
        memset(session, 0, sizeof(*session));
        return ZB_OK;
    }
    if (s->count == s->capacity) {
        size_t cap = s->capacity != 0 ? s->capacity * 2 : 8;
        zb_session *grown;
        if (cap > (size_t)-1 / sizeof(*grown)) {
            return zb_error_nomem(err);
        }
        grown = zb_realloc(s->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return zb_error_nomem(err);
        }
        s->items = grown;
        s->capacity = cap;
    }
    s->items[s->count++] = *session;
    memset(session, 0, sizeof(*session));
    return ZB_OK;
}

int zb_sessions_remove(zb_sessions *s, const char *token)
{
    size_t i;
    if (s == NULL || token == NULL) {
        return 0;
    }
    for (i = 0; i < s->count; i++) {
        if (s->items[i].token != NULL && strcmp(s->items[i].token, token) == 0) {
            zb_session_free(&s->items[i]);
            if (i + 1 < s->count) {
                memmove(&s->items[i], &s->items[i + 1],
                        (s->count - i - 1) * sizeof(s->items[0]));
            }
            s->count--;
            return 1;
        }
    }
    return 0;
}

size_t zb_sessions_prune_stale(zb_sessions *s, int64_t now)
{
    const int64_t ttl = (int64_t)ZB_UPLOAD_SESSION_TTL_MINUTES * 60;
    size_t removed = 0;
    size_t i = 0;

    while (i < s->count) {
        /* The server discards idle sessions after 3 hours, so anything older
         * than that can never be resumed and is just clutter. */
        if (now - s->items[i].updated_at > ttl) {
            zb_session_free(&s->items[i]);
            if (i + 1 < s->count) {
                memmove(&s->items[i], &s->items[i + 1],
                        (s->count - i - 1) * sizeof(s->items[0]));
            }
            s->count--;
            removed++;
            continue;
        }
        i++;
    }
    return removed;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                          */
/* ------------------------------------------------------------------ */

static zb_status parse_one(const zb_json *node, zb_session *out, zb_error *err)
{
    const char *s = NULL;
    const zb_json *sent;
    size_t n;
    size_t i;
    int64_t updated = 0;

    memset(out, 0, sizeof(*out));
    if (!zb_json_is_object(node) || !zb_json_get_str(node, "token", &s)) {
        return ZB_ERR_PROTO;
    }

#define TAKE(key, field)                                                       \
    do {                                                                       \
        if (zb_json_get_str(node, key, &s)) {                                  \
            out->field = zb_strdup(s);                                         \
            if (out->field == NULL) {                                          \
                zb_session_free(out);                                          \
                return zb_error_nomem(err);                                    \
            }                                                                  \
        }                                                                      \
    } while (0)
    TAKE("token", token);
    TAKE("deleteToken", delete_token);
    TAKE("sourcePath", source_path);
    TAKE("filename", filename);
    TAKE("mimeType", mime_type);
    TAKE("collectionToken", collection_token);
#undef TAKE

    (void)zb_json_get_u64(node, "size", &out->size);
    (void)zb_json_get_i64(node, "mtime", &out->mtime);
    (void)zb_json_get_u64(node, "chunkSize", &out->chunk_size);
    if (zb_json_get_i64(node, "updatedAt", &updated)) {
        out->updated_at = updated;
    }

    {
        uint64_t total = 0;
        if (!zb_json_get_u64(node, "totalChunks", &total) ||
            zb_session_alloc_bitmap(out, total) != 0) {
            zb_session_free(out);
            return ZB_ERR_PROTO;
        }
    }

    sent = zb_json_get(node, "sentChunks");
    n = zb_json_array_len(sent);
    for (i = 0; i < n; i++) {
        uint64_t index = 0;
        if (zb_json_as_u64(zb_json_at(sent, i), &index) &&
            index < out->total_chunks) {
            zb_session_mark(out, index);
        }
    }
    return ZB_OK;
}

zb_status zb_sessions_load(zb_sessions *s, const char *base_dir, zb_error *err)
{
    char *path;
    char *text = NULL;
    size_t len = 0;
    zb_json *root = NULL;
    const zb_json *items;
    size_t n;
    size_t i;
    int64_t version = 0;
    zb_status rc = ZB_OK;

    zb_sessions_init(s);

    path = zb_paths_file(base_dir, ZB_SESSIONS_FILE_NAME);
    if (path == NULL) {
        return zb_error_nomem(err);
    }
    rc = zb_read_file(path, &text, &len, err);
    if (rc != ZB_OK || text == NULL || len == 0) {
        goto cleanup;
    }

    root = zb_json_parse(text, len);
    if (root == NULL || !zb_json_is_object(root)) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "%s is not valid JSON. It has been left untouched — "
                           "delete it to start fresh (you will lose the ability "
                           "to resume any interrupted upload).",
                           path);
        goto cleanup;
    }
    if (!zb_json_get_i64(root, "version", &version) ||
        version != ZB_SESSIONS_VERSION) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "%s has an unrecognized version; left untouched.",
                           path);
        goto cleanup;
    }

    items = zb_json_get(root, "sessions");
    n = zb_json_array_len(items);
    for (i = 0; i < n; i++) {
        zb_session session;
        zb_status one = parse_one(zb_json_at(items, i), &session, err);
        if (one == ZB_ERR_NOMEM) {
            rc = one;
            goto cleanup;
        }
        if (one != ZB_OK) {
            continue; /* skip an unusable record rather than refusing to run */
        }
        rc = zb_sessions_put(s, &session, err);
        if (rc != ZB_OK) {
            zb_session_free(&session);
            goto cleanup;
        }
    }

cleanup:
    if (rc != ZB_OK) {
        zb_sessions_free(s);
    }
    zb_json_free(root);
    zb_free(text);
    zb_free(path);
    return rc;
}

zb_status zb_sessions_save(const zb_sessions *s, const char *base_dir,
                           zb_error *err)
{
    zb_json *root;
    zb_json *array;
    char *text = NULL;
    char *path = NULL;
    size_t i;
    zb_status rc = ZB_OK;

    root = zb_json_new_object();
    array = zb_json_new_array();
    if (root == NULL || array == NULL) {
        zb_json_free(root);
        zb_json_free(array);
        return zb_error_nomem(err);
    }
    if (zb_json_obj_set_i64(root, "version", ZB_SESSIONS_VERSION) != 0) {
        zb_json_free(array);
        rc = zb_error_nomem(err);
        goto cleanup;
    }

    for (i = 0; i < s->count; i++) {
        const zb_session *item = &s->items[i];
        zb_json *obj = zb_json_new_object();
        zb_json *sent = zb_json_new_array();
        uint64_t k;
        int failed;

        if (obj == NULL || sent == NULL) {
            zb_json_free(obj);
            zb_json_free(sent);
            zb_json_free(array);
            rc = zb_error_nomem(err);
            goto cleanup;
        }
        failed = zb_json_obj_set_str(obj, "token", item->token) != 0 ||
                 zb_json_obj_set_str(obj, "deleteToken", item->delete_token) !=
                     0 ||
                 zb_json_obj_set_str(obj, "sourcePath", item->source_path) !=
                     0 ||
                 zb_json_obj_set_str(obj, "filename", item->filename) != 0 ||
                 zb_json_obj_set_str(obj, "mimeType", item->mime_type) != 0 ||
                 zb_json_obj_set_str(obj, "collectionToken",
                                     item->collection_token) != 0 ||
                 zb_json_obj_set_u64(obj, "size", item->size) != 0 ||
                 zb_json_obj_set_i64(obj, "mtime", item->mtime) != 0 ||
                 zb_json_obj_set_u64(obj, "chunkSize", item->chunk_size) != 0 ||
                 zb_json_obj_set_u64(obj, "totalChunks", item->total_chunks) !=
                     0 ||
                 zb_json_obj_set_i64(obj, "updatedAt", item->updated_at) != 0;
        if (failed) {
            zb_json_free(obj);
            zb_json_free(sent);
            zb_json_free(array);
            rc = zb_error_nomem(err);
            goto cleanup;
        }

        for (k = 0; k < item->total_chunks; k++) {
            if (zb_session_has(item, k) &&
                zb_json_arr_add(sent, zb_json_new_u64(k)) != 0) {
                zb_json_free(obj);
                zb_json_free(sent);
                zb_json_free(array);
                rc = zb_error_nomem(err);
                goto cleanup;
            }
        }
        /* obj_add and arr_add each free the child they were handed on
         * failure, so only the container still needs freeing here. */
        if (zb_json_obj_add(obj, "sentChunks", sent) != 0) {
            zb_json_free(obj);
            zb_json_free(array);
            rc = zb_error_nomem(err);
            goto cleanup;
        }
        if (zb_json_arr_add(array, obj) != 0) {
            zb_json_free(array);
            rc = zb_error_nomem(err);
            goto cleanup;
        }
    }

    if (zb_json_obj_add(root, "sessions", array) != 0) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    text = zb_json_print(root, 1);
    if (text == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    path = zb_paths_file(base_dir, ZB_SESSIONS_FILE_NAME);
    if (path == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    rc = zb_atomic_write(path, text, strlen(text), err);

cleanup:
    if (text != NULL) {
        zb_free_secret(text); /* holds delete tokens */
    }
    zb_free(path);
    zb_json_free(root);
    return rc;
}
