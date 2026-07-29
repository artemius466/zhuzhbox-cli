#include "store/shelf.h"

#include <stdlib.h>
#include <string.h>

#include "format/time.h"
#include "store/atomic.h"
#include "store/paths.h"
#include "util/buf.h"
#include "util/str.h"

void zb_shelf_init(zb_shelf *shelf)
{
    memset(shelf, 0, sizeof(*shelf));
}

void zb_shelf_entry_free(zb_shelf_entry *entry)
{
    if (entry == NULL) {
        return;
    }
    zb_free(entry->token);
    /* The delete token is a capability: wipe it rather than leaving it in a
     * freed block that may be handed to something else later. */
    zb_free_secret(entry->delete_token);
    zb_free(entry->kind);
    zb_free(entry->name);
    zb_free(entry->url);
    zb_free(entry->uploaded_at);
    zb_free(entry->expires_at);
    memset(entry, 0, sizeof(*entry));
}

void zb_shelf_free(zb_shelf *shelf)
{
    size_t i;
    if (shelf == NULL) {
        return;
    }
    for (i = 0; i < shelf->count; i++) {
        zb_shelf_entry_free(&shelf->entries[i]);
    }
    zb_free(shelf->entries);
    zb_shelf_init(shelf);
}

static int copy_entry(zb_shelf_entry *dst, const zb_shelf_entry *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->size = src->size;
    dst->token = zb_strdup(src->token);
    if (dst->token == NULL) {
        return -1;
    }
    /* Every other field is optional; only fail when a copy that was asked for
     * could not be made. */
#define COPY_OPT(field)                                                        \
    do {                                                                       \
        if (src->field != NULL) {                                              \
            dst->field = zb_strdup(src->field);                                \
            if (dst->field == NULL) {                                          \
                zb_shelf_entry_free(dst);                                      \
                return -1;                                                     \
            }                                                                  \
        }                                                                      \
    } while (0)
    COPY_OPT(delete_token);
    COPY_OPT(kind);
    COPY_OPT(name);
    COPY_OPT(url);
    COPY_OPT(uploaded_at);
    COPY_OPT(expires_at);
#undef COPY_OPT
    return 0;
}

zb_shelf_entry *zb_shelf_find(zb_shelf *shelf, const char *token)
{
    size_t i;
    if (shelf == NULL || token == NULL) {
        return NULL;
    }
    for (i = 0; i < shelf->count; i++) {
        if (shelf->entries[i].token != NULL &&
            strcmp(shelf->entries[i].token, token) == 0) {
            return &shelf->entries[i];
        }
    }
    return NULL;
}

static zb_status reserve(zb_shelf *shelf, zb_error *err)
{
    size_t cap;
    zb_shelf_entry *grown;

    if (shelf->count < shelf->capacity) {
        return ZB_OK;
    }
    cap = shelf->capacity != 0 ? shelf->capacity * 2 : 16;
    if (cap > (size_t)-1 / sizeof(*grown)) {
        return zb_error_nomem(err);
    }
    grown = zb_realloc(shelf->entries, cap * sizeof(*grown));
    if (grown == NULL) {
        return zb_error_nomem(err);
    }
    shelf->entries = grown;
    shelf->capacity = cap;
    return ZB_OK;
}

zb_status zb_shelf_upsert(zb_shelf *shelf, const zb_shelf_entry *entry,
                          zb_error *err)
{
    zb_shelf_entry *existing;
    zb_shelf_entry copy;

    if (entry->token == NULL || entry->token[0] == '\0') {
        return zb_error_setf(err, ZB_ERR_PROTO,
                             "cannot record an upload with no token");
    }
    if (copy_entry(&copy, entry) != 0) {
        return zb_error_nomem(err);
    }

    existing = zb_shelf_find(shelf, entry->token);
    if (existing != NULL) {
        zb_shelf_entry_free(existing);
        *existing = copy;
        return ZB_OK;
    }
    if (reserve(shelf, err) != ZB_OK) {
        zb_shelf_entry_free(&copy);
        return err->status;
    }
    shelf->entries[shelf->count++] = copy;
    return ZB_OK;
}

int zb_shelf_remove(zb_shelf *shelf, const char *token)
{
    size_t i;
    if (shelf == NULL || token == NULL) {
        return 0;
    }
    for (i = 0; i < shelf->count; i++) {
        if (shelf->entries[i].token != NULL &&
            strcmp(shelf->entries[i].token, token) == 0) {
            zb_shelf_entry_free(&shelf->entries[i]);
            if (i + 1 < shelf->count) {
                memmove(&shelf->entries[i], &shelf->entries[i + 1],
                        (shelf->count - i - 1) * sizeof(shelf->entries[0]));
            }
            shelf->count--;
            return 1;
        }
    }
    return 0;
}

zb_status zb_shelf_load(zb_shelf *shelf, const char *base_dir, zb_error *err)
{
    char *path;
    char *text = NULL;
    size_t len = 0;
    zb_json *root = NULL;
    const zb_json *entries;
    size_t count;
    size_t i;
    int64_t version = 0;
    zb_status rc;

    zb_shelf_init(shelf);

    path = zb_paths_file(base_dir, ZB_SHELF_FILE_NAME);
    if (path == NULL) {
        return zb_error_nomem(err);
    }

    rc = zb_read_file(path, &text, &len, err);
    if (rc != ZB_OK) {
        goto cleanup;
    }
    if (text == NULL || len == 0) {
        /* No shelf yet is simply an empty shelf. */
        goto cleanup;
    }

    root = zb_json_parse(text, len);
    if (root == NULL || !zb_json_is_object(root)) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "%s is not valid JSON. It holds your delete tokens, "
                           "so it has been left untouched — fix or move it, "
                           "then try again.",
                           path);
        goto cleanup;
    }

    if (!zb_json_get_i64(root, "version", &version) ||
        version != ZB_SHELF_VERSION) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "%s has an unrecognized version. It has been left "
                           "untouched — this build understands version %d.",
                           path, ZB_SHELF_VERSION);
        goto cleanup;
    }

    entries = zb_json_get(root, "entries");
    if (entries != NULL && !zb_json_is_array(entries)) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "%s: \"entries\" is not a list. Left untouched.",
                           path);
        goto cleanup;
    }

    count = zb_json_array_len(entries);
    for (i = 0; i < count; i++) {
        const zb_json *node = zb_json_at(entries, i);
        zb_shelf_entry entry;

        memset(&entry, 0, sizeof(entry));
        if (!zb_json_is_object(node)) {
            continue;
        }
        {
            const char *s = NULL;

            if (!zb_json_get_str(node, "token", &s)) {
                continue; /* an entry without a token is not usable */
            }
/* An allocation failure here would silently drop a delete token, so every
 * copy is checked even though the field itself is optional. */
#define TAKE(key, field)                                                       \
    do {                                                                       \
        if (zb_json_get_str(node, key, &s)) {                                  \
            entry.field = zb_strdup(s);                                        \
            if (entry.field == NULL) {                                         \
                zb_shelf_entry_free(&entry);                                   \
                rc = zb_error_nomem(err);                                      \
                goto cleanup;                                                  \
            }                                                                  \
        }                                                                      \
    } while (0)
            TAKE("token", token);
            TAKE("deleteToken", delete_token);
            TAKE("kind", kind);
            TAKE("name", name);
            TAKE("url", url);
            TAKE("uploadedAt", uploaded_at);
            TAKE("expiresAt", expires_at);
#undef TAKE
            (void)zb_json_get_u64(node, "size", &entry.size);
        }

        rc = reserve(shelf, err);
        if (rc != ZB_OK) {
            zb_shelf_entry_free(&entry);
            goto cleanup;
        }
        shelf->entries[shelf->count++] = entry;
    }

cleanup:
    if (rc != ZB_OK) {
        zb_shelf_free(shelf);
    }
    zb_json_free(root);
    zb_free(text);
    zb_free(path);
    return rc;
}

zb_json *zb_shelf_entry_json(const zb_shelf_entry *entry,
                             int include_delete_token)
{
    zb_json *obj = zb_json_new_object();
    if (obj == NULL) {
        return NULL;
    }
    if (zb_json_obj_set_str(obj, "token", entry->token) != 0 ||
        zb_json_obj_set_str(obj, "kind",
                            entry->kind != NULL ? entry->kind : "file") != 0 ||
        zb_json_obj_set_str(obj, "name", entry->name) != 0 ||
        zb_json_obj_set_u64(obj, "size", entry->size) != 0 ||
        zb_json_obj_set_str(obj, "url", entry->url) != 0 ||
        zb_json_obj_set_str(obj, "uploadedAt", entry->uploaded_at) != 0 ||
        zb_json_obj_set_str(obj, "expiresAt", entry->expires_at) != 0) {
        zb_json_free(obj);
        return NULL;
    }
    if (include_delete_token &&
        zb_json_obj_set_str(obj, "deleteToken", entry->delete_token) != 0) {
        zb_json_free(obj);
        return NULL;
    }
    return obj;
}

zb_status zb_shelf_save(const zb_shelf *shelf, const char *base_dir,
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
    if (zb_json_obj_set_i64(root, "version", ZB_SHELF_VERSION) != 0) {
        rc = zb_error_nomem(err);
        zb_json_free(array);
        goto cleanup;
    }

    for (i = 0; i < shelf->count; i++) {
        /* The on-disk shelf is the one place the delete token belongs. */
        zb_json *item = zb_shelf_entry_json(&shelf->entries[i], 1);
        if (item == NULL || zb_json_arr_add(array, item) != 0) {
            rc = zb_error_nomem(err);
            zb_json_free(array);
            goto cleanup;
        }
    }
    if (zb_json_obj_add(root, "entries", array) != 0) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }

    text = zb_json_print(root, 1);
    if (text == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    path = zb_paths_file(base_dir, ZB_SHELF_FILE_NAME);
    if (path == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    rc = zb_atomic_write(path, text, strlen(text), err);

cleanup:
    if (text != NULL) {
        /* The serialized shelf contains every delete token. */
        zb_free_secret(text);
    }
    zb_free(path);
    zb_json_free(root);
    return rc;
}

zb_status zb_shelf_record(const char *base_dir, const zb_shelf_entry *entry,
                          zb_error *err)
{
    zb_shelf shelf;
    zb_status rc;

    rc = zb_shelf_load(&shelf, base_dir, err);
    if (rc != ZB_OK) {
        return rc;
    }
    rc = zb_shelf_upsert(&shelf, entry, err);
    if (rc == ZB_OK) {
        rc = zb_shelf_save(&shelf, base_dir, err);
    }
    zb_shelf_free(&shelf);
    return rc;
}

zb_status zb_shelf_forget(const char *base_dir, const char *token,
                          zb_error *err)
{
    zb_shelf shelf;
    zb_status rc;

    rc = zb_shelf_load(&shelf, base_dir, err);
    if (rc != ZB_OK) {
        return rc;
    }
    if (zb_shelf_remove(&shelf, token)) {
        rc = zb_shelf_save(&shelf, base_dir, err);
    }
    zb_shelf_free(&shelf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Sorting                                                              */
/* ------------------------------------------------------------------ */

static int64_t entry_epoch(const char *iso)
{
    int64_t epoch = 0;
    if (iso == NULL || zb_time_parse_iso8601(iso, &epoch) != 0) {
        return 0;
    }
    return epoch;
}

/* Comparators are total, and never subtract two uint64_t into an int. */
static int cmp_newest(const void *a, const void *b)
{
    const zb_shelf_entry *ea = a;
    const zb_shelf_entry *eb = b;
    int64_t ta = entry_epoch(ea->uploaded_at);
    int64_t tb = entry_epoch(eb->uploaded_at);

    if (ta != tb) {
        return ta > tb ? -1 : 1;
    }
    return strcmp(ea->token != NULL ? ea->token : "",
                  eb->token != NULL ? eb->token : "");
}

static int cmp_name(const void *a, const void *b)
{
    const zb_shelf_entry *ea = a;
    const zb_shelf_entry *eb = b;
    int r = strcmp(ea->name != NULL ? ea->name : "",
                   eb->name != NULL ? eb->name : "");
    if (r != 0) {
        return r;
    }
    return strcmp(ea->token != NULL ? ea->token : "",
                  eb->token != NULL ? eb->token : "");
}

static int cmp_size(const void *a, const void *b)
{
    const zb_shelf_entry *ea = a;
    const zb_shelf_entry *eb = b;

    if (ea->size != eb->size) {
        return ea->size > eb->size ? -1 : 1;
    }
    return strcmp(ea->token != NULL ? ea->token : "",
                  eb->token != NULL ? eb->token : "");
}

static int cmp_expires(const void *a, const void *b)
{
    const zb_shelf_entry *ea = a;
    const zb_shelf_entry *eb = b;
    int64_t ta = entry_epoch(ea->expires_at);
    int64_t tb = entry_epoch(eb->expires_at);

    if (ta != tb) {
        return ta < tb ? -1 : 1; /* soonest first */
    }
    return strcmp(ea->token != NULL ? ea->token : "",
                  eb->token != NULL ? eb->token : "");
}

void zb_shelf_sort_entries(zb_shelf *shelf, zb_shelf_sort key)
{
    int (*cmp)(const void *, const void *);

    if (shelf == NULL || shelf->count < 2) {
        return;
    }
    switch (key) {
    case ZB_SHELF_SORT_NAME:
        cmp = cmp_name;
        break;
    case ZB_SHELF_SORT_SIZE:
        cmp = cmp_size;
        break;
    case ZB_SHELF_SORT_EXPIRES:
        cmp = cmp_expires;
        break;
    case ZB_SHELF_SORT_NEWEST:
    default:
        cmp = cmp_newest;
        break;
    }
    qsort(shelf->entries, shelf->count, sizeof(shelf->entries[0]), cmp);
}
