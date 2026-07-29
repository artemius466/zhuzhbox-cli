/* json.h — the whole tree's JSON surface.
 *
 * util/json.c is the ONLY file that includes cJSON; everything else talks to
 * these functions. Keeping the boundary here means a different parser could be
 * swapped in without touching a command.
 *
 * Ownership: a `zb_json *` returned by a _new_/_parse_ function is owned by the
 * caller and freed with zb_json_free(). A `const zb_json *` returned by a
 * lookup borrows from its parent and must not be freed. Functions that add a
 * child to a container take ownership of the child on success, and free it on
 * failure, so a caller can chain them without leaking.
 */
#ifndef ZB_UTIL_JSON_H
#define ZB_UTIL_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef struct zb_json zb_json;

/* ---------- lifecycle ---------- */

/* Parse `len` bytes. Returns NULL on malformed input or OOM. */
zb_json *zb_json_parse(const char *text, size_t len);

void zb_json_free(zb_json *node);

/* Serialize. `pretty` adds newlines and indentation. Caller frees the result
 * with zb_free(). NULL on OOM. */
char *zb_json_print(const zb_json *node, int pretty);

/* ---------- type tests ---------- */

int zb_json_is_object(const zb_json *node);
int zb_json_is_array(const zb_json *node);
int zb_json_is_string(const zb_json *node);
int zb_json_is_number(const zb_json *node);
int zb_json_is_bool(const zb_json *node);
int zb_json_is_null(const zb_json *node);

/* ---------- reading ---------- */

/* Borrowed child, or NULL if absent. */
const zb_json *zb_json_get(const zb_json *obj, const char *key);

/* Borrowed string, or NULL if the value is missing or not a string. */
const char *zb_json_str(const zb_json *node);

/* Convenience lookups. Each returns 1 on success and 0 if the key is missing
 * or has the wrong type; *out is untouched on failure.
 *
 * The number readers reject non-finite values, negatives, and anything above
 * 2^53 rather than truncating a double into an integer (§2.1). */
int zb_json_get_str(const zb_json *obj, const char *key, const char **out);
int zb_json_get_u64(const zb_json *obj, const char *key, uint64_t *out);
int zb_json_get_i64(const zb_json *obj, const char *key, int64_t *out);
int zb_json_get_double(const zb_json *obj, const char *key, double *out);
int zb_json_get_bool(const zb_json *obj, const char *key, int *out);

/* Same validation, applied to a bare node rather than an object member. */
int zb_json_as_u64(const zb_json *node, uint64_t *out);
int zb_json_as_bool(const zb_json *node, int *out);

/* Arrays. zb_json_at() returns NULL when the index is out of range. */
size_t zb_json_array_len(const zb_json *arr);
const zb_json *zb_json_at(const zb_json *arr, size_t index);

/* ---------- building ---------- */

zb_json *zb_json_new_object(void);
zb_json *zb_json_new_array(void);
zb_json *zb_json_new_string(const char *s);
zb_json *zb_json_new_u64(uint64_t v);
zb_json *zb_json_new_double(double v);
zb_json *zb_json_new_bool(int v);
zb_json *zb_json_new_null(void);

/* All return 0 on success, -1 on failure. On failure `child` is freed. */
int zb_json_obj_add(zb_json *obj, const char *key, zb_json *child);
int zb_json_arr_add(zb_json *arr, zb_json *child);

/* Shorthands; same return convention. A NULL string value adds a JSON null,
 * which is what every optional field in this API wants. */
int zb_json_obj_set_str(zb_json *obj, const char *key, const char *value);
int zb_json_obj_set_u64(zb_json *obj, const char *key, uint64_t value);
int zb_json_obj_set_i64(zb_json *obj, const char *key, int64_t value);
int zb_json_obj_set_bool(zb_json *obj, const char *key, int value);

/* Detach a child from an object, transferring ownership to the caller.
 * Returns NULL if the key is absent. */
zb_json *zb_json_obj_detach(zb_json *obj, const char *key);

/* Deep copy. Caller owns the result. */
zb_json *zb_json_clone(const zb_json *node);

#endif /* ZB_UTIL_JSON_H */
