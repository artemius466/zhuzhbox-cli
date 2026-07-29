/* json.c — the only file in the tree that includes cJSON. */

#include "util/json.h"

#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "util/buf.h"

/* zb_json is an opaque handle for cJSON. The casts are confined to this file. */
static cJSON *j(zb_json *n)
{
    return (cJSON *)(void *)n;
}

static const cJSON *cj(const zb_json *n)
{
    return (const cJSON *)(const void *)n;
}

static zb_json *wrap(cJSON *n)
{
    return (zb_json *)(void *)n;
}

static const zb_json *wrapc(const cJSON *n)
{
    return (const zb_json *)(const void *)n;
}

/* cJSON stores every number as a double. Integers below 2^53 are exact, which
 * covers byte counts well past the 25 GiB cap, but anything outside that range
 * or non-finite is a protocol error rather than something to cast and hope. */
#define ZB_JSON_MAX_EXACT_INT 9007199254740992.0

static int number_to_u64(double v, uint64_t *out)
{
    if (!isfinite(v)) {
        return 0;
    }
    if (v < 0.0 || v > ZB_JSON_MAX_EXACT_INT) {
        return 0;
    }
    if (v != floor(v)) {
        return 0;
    }
    *out = (uint64_t)v;
    return 1;
}

static int number_to_i64(double v, int64_t *out)
{
    if (!isfinite(v)) {
        return 0;
    }
    if (v < -ZB_JSON_MAX_EXACT_INT || v > ZB_JSON_MAX_EXACT_INT) {
        return 0;
    }
    if (v != floor(v)) {
        return 0;
    }
    *out = (int64_t)v;
    return 1;
}

zb_json *zb_json_parse(const char *text, size_t len)
{
    cJSON *root;
    if (text == NULL) {
        return NULL;
    }
    root = cJSON_ParseWithLength(text, len);
    return wrap(root);
}

void zb_json_free(zb_json *node)
{
    cJSON_Delete(j(node));
}

char *zb_json_print(const zb_json *node, int pretty)
{
    char *raw;
    char *out;

    if (node == NULL) {
        return zb_strdup("null");
    }
    raw = pretty ? cJSON_Print(cj(node)) : cJSON_PrintUnformatted(cj(node));
    if (raw == NULL) {
        return NULL;
    }
    /* Hand back memory the caller can zb_free(), rather than making every
     * caller remember that this one string came from cJSON's allocator. */
    out = zb_strdup(raw);
    cJSON_free(raw);
    return out;
}

int zb_json_is_object(const zb_json *node)
{
    return node != NULL && cJSON_IsObject(cj(node));
}

int zb_json_is_array(const zb_json *node)
{
    return node != NULL && cJSON_IsArray(cj(node));
}

int zb_json_is_string(const zb_json *node)
{
    return node != NULL && cJSON_IsString(cj(node));
}

int zb_json_is_number(const zb_json *node)
{
    return node != NULL && cJSON_IsNumber(cj(node));
}

int zb_json_is_bool(const zb_json *node)
{
    return node != NULL && cJSON_IsBool(cj(node));
}

int zb_json_is_null(const zb_json *node)
{
    return node != NULL && cJSON_IsNull(cj(node));
}

const zb_json *zb_json_get(const zb_json *obj, const char *key)
{
    if (obj == NULL || key == NULL || !cJSON_IsObject(cj(obj))) {
        return NULL;
    }
    return wrapc(cJSON_GetObjectItemCaseSensitive(cj(obj), key));
}

const char *zb_json_str(const zb_json *node)
{
    if (node == NULL || !cJSON_IsString(cj(node))) {
        return NULL;
    }
    return cj(node)->valuestring;
}

int zb_json_get_str(const zb_json *obj, const char *key, const char **out)
{
    const zb_json *v = zb_json_get(obj, key);
    const char *s = zb_json_str(v);
    if (s == NULL) {
        return 0;
    }
    *out = s;
    return 1;
}

int zb_json_as_u64(const zb_json *node, uint64_t *out)
{
    if (node == NULL || !cJSON_IsNumber(cj(node))) {
        return 0;
    }
    return number_to_u64(cj(node)->valuedouble, out);
}

int zb_json_as_bool(const zb_json *node, int *out)
{
    if (node == NULL || !cJSON_IsBool(cj(node))) {
        return 0;
    }
    *out = cJSON_IsTrue(cj(node)) ? 1 : 0;
    return 1;
}

int zb_json_get_u64(const zb_json *obj, const char *key, uint64_t *out)
{
    return zb_json_as_u64(zb_json_get(obj, key), out);
}

int zb_json_get_i64(const zb_json *obj, const char *key, int64_t *out)
{
    const zb_json *v = zb_json_get(obj, key);
    if (v == NULL || !cJSON_IsNumber(cj(v))) {
        return 0;
    }
    return number_to_i64(cj(v)->valuedouble, out);
}

int zb_json_get_double(const zb_json *obj, const char *key, double *out)
{
    const zb_json *v = zb_json_get(obj, key);
    if (v == NULL || !cJSON_IsNumber(cj(v)) || !isfinite(cj(v)->valuedouble)) {
        return 0;
    }
    *out = cj(v)->valuedouble;
    return 1;
}

int zb_json_get_bool(const zb_json *obj, const char *key, int *out)
{
    const zb_json *v = zb_json_get(obj, key);
    if (v == NULL || !cJSON_IsBool(cj(v))) {
        return 0;
    }
    *out = cJSON_IsTrue(cj(v)) ? 1 : 0;
    return 1;
}

size_t zb_json_array_len(const zb_json *arr)
{
    int n;
    if (arr == NULL || !cJSON_IsArray(cj(arr))) {
        return 0;
    }
    n = cJSON_GetArraySize(cj(arr));
    return n < 0 ? 0 : (size_t)n;
}

const zb_json *zb_json_at(const zb_json *arr, size_t index)
{
    if (arr == NULL || !cJSON_IsArray(cj(arr)) || index > (size_t)INT32_MAX) {
        return NULL;
    }
    return wrapc(cJSON_GetArrayItem(cj(arr), (int)index));
}

zb_json *zb_json_new_object(void)
{
    return wrap(cJSON_CreateObject());
}

zb_json *zb_json_new_array(void)
{
    return wrap(cJSON_CreateArray());
}

zb_json *zb_json_new_string(const char *s)
{
    if (s == NULL) {
        return wrap(cJSON_CreateNull());
    }
    return wrap(cJSON_CreateString(s));
}

zb_json *zb_json_new_u64(uint64_t v)
{
    return wrap(cJSON_CreateNumber((double)v));
}

zb_json *zb_json_new_double(double v)
{
    return wrap(cJSON_CreateNumber(v));
}

zb_json *zb_json_new_bool(int v)
{
    return wrap(cJSON_CreateBool(v ? 1 : 0));
}

zb_json *zb_json_new_null(void)
{
    return wrap(cJSON_CreateNull());
}

int zb_json_obj_add(zb_json *obj, const char *key, zb_json *child)
{
    if (child == NULL) {
        return -1;
    }
    if (obj == NULL || key == NULL || !cJSON_IsObject(cj(obj))) {
        cJSON_Delete(j(child));
        return -1;
    }
    if (!cJSON_AddItemToObject(j(obj), key, j(child))) {
        cJSON_Delete(j(child));
        return -1;
    }
    return 0;
}

int zb_json_arr_add(zb_json *arr, zb_json *child)
{
    if (child == NULL) {
        return -1;
    }
    if (arr == NULL || !cJSON_IsArray(cj(arr))) {
        cJSON_Delete(j(child));
        return -1;
    }
    if (!cJSON_AddItemToArray(j(arr), j(child))) {
        cJSON_Delete(j(child));
        return -1;
    }
    return 0;
}

int zb_json_obj_set_str(zb_json *obj, const char *key, const char *value)
{
    return zb_json_obj_add(obj, key, zb_json_new_string(value));
}

int zb_json_obj_set_u64(zb_json *obj, const char *key, uint64_t value)
{
    return zb_json_obj_add(obj, key, zb_json_new_u64(value));
}

int zb_json_obj_set_i64(zb_json *obj, const char *key, int64_t value)
{
    return zb_json_obj_add(obj, key, zb_json_new_double((double)value));
}

int zb_json_obj_set_bool(zb_json *obj, const char *key, int value)
{
    return zb_json_obj_add(obj, key, zb_json_new_bool(value));
}

zb_json *zb_json_obj_detach(zb_json *obj, const char *key)
{
    if (obj == NULL || key == NULL || !cJSON_IsObject(cj(obj))) {
        return NULL;
    }
    return wrap(cJSON_DetachItemFromObjectCaseSensitive(j(obj), key));
}

zb_json *zb_json_clone(const zb_json *node)
{
    if (node == NULL) {
        return NULL;
    }
    return wrap(cJSON_Duplicate(cj(node), 1));
}
