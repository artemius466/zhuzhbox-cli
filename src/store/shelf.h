/* shelf.h — "my uploads", the local mirror of what the website keeps in
 * localStorage.
 *
 * This file is the only record that a given upload is yours. The server cannot
 * tell you: uploads are encrypted at rest with keys derived from the token in
 * the share link, so it does not know who uploaded what and cannot hand you
 * back a delete token. Losing shelf.json means losing the ability to delete
 * anything whose delete token you did not write down. Treat it accordingly:
 * atomic writes, 0600, and never a silent reset of a file we failed to parse.
 */
#ifndef ZB_STORE_SHELF_H
#define ZB_STORE_SHELF_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"
#include "util/json.h"

#define ZB_SHELF_VERSION 1

typedef struct {
    char *token;
    char *delete_token; /* capability secret — see zb_shelf_entry_json() */
    char *kind;         /* "file" or "collection" */
    char *name;         /* filename, or collection title */
    uint64_t size;
    char *url;
    char *uploaded_at; /* ISO-8601, as the server sent it */
    char *expires_at;
} zb_shelf_entry;

typedef struct {
    zb_shelf_entry *entries;
    size_t count;
    size_t capacity;
} zb_shelf;

typedef enum {
    ZB_SHELF_SORT_NEWEST = 0,
    ZB_SHELF_SORT_NAME,
    ZB_SHELF_SORT_SIZE,
    ZB_SHELF_SORT_EXPIRES
} zb_shelf_sort;

void zb_shelf_init(zb_shelf *shelf);
void zb_shelf_free(zb_shelf *shelf);

/* Load from `base_dir`/shelf.json.
 *
 * A missing file is an empty shelf. Malformed JSON, or a version we do not
 * know, is an error naming the path — and the file is left exactly as it was. */
zb_status zb_shelf_load(zb_shelf *shelf, const char *base_dir, zb_error *err);

/* Write the whole shelf atomically. Build the new state in memory first: a
 * half-written prune is worse than no prune. */
zb_status zb_shelf_save(const zb_shelf *shelf, const char *base_dir,
                        zb_error *err);

/* Borrowed pointer into the shelf, or NULL. */
zb_shelf_entry *zb_shelf_find(zb_shelf *shelf, const char *token);

/* Insert or replace by token. All strings are copied. */
zb_status zb_shelf_upsert(zb_shelf *shelf, const zb_shelf_entry *entry,
                          zb_error *err);

/* 1 if an entry was removed. The delete token is zeroed before being freed. */
int zb_shelf_remove(zb_shelf *shelf, const char *token);

/* Load, upsert, save — the write-through used after every successful upload so
 * a crash a moment later cannot lose the delete token. */
zb_status zb_shelf_record(const char *base_dir, const zb_shelf_entry *entry,
                          zb_error *err);

/* Load, remove, save. Absent tokens are not an error. */
zb_status zb_shelf_forget(const char *base_dir, const char *token,
                          zb_error *err);

void zb_shelf_sort_entries(zb_shelf *shelf, zb_shelf_sort key);

/* Serialize one entry. `include_delete_token` must be an explicit decision at
 * every call site — it is off in every default view. */
zb_json *zb_shelf_entry_json(const zb_shelf_entry *entry,
                             int include_delete_token);

/* Free the strings of a standalone entry (not one owned by a zb_shelf). */
void zb_shelf_entry_free(zb_shelf_entry *entry);

#endif /* ZB_STORE_SHELF_H */
