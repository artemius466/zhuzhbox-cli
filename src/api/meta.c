#include "api/meta.h"

#include <string.h>

#include "zb_limits.h"
#include "util/buf.h"
#include "util/str.h"

zb_status zb_meta_get(zb_client *client, const char *token, zb_json **out,
                      zb_error *err)
{
    char *path = zb_asprintf("/v1/upload/%s", token);
    zb_status rc;

    if (path == NULL) {
        return zb_error_nomem(err);
    }
    rc = zb_client_request(client, "GET", path, NULL, NULL, out, NULL, err);
    zb_free(path);
    return rc;
}

zb_status zb_meta_delete(zb_client *client, const char *token,
                         const char *delete_token, long *out_status,
                         zb_error *err)
{
    char *path;
    char *header;
    const char *headers[2];
    zb_status rc;

    path = zb_asprintf("/v1/upload/%s", token);
    if (path == NULL) {
        return zb_error_nomem(err);
    }
    header = zb_asprintf("X-Delete-Token: %s",
                         delete_token != NULL ? delete_token : "");
    if (header == NULL) {
        zb_free(path);
        return zb_error_nomem(err);
    }
    headers[0] = header;
    headers[1] = NULL;

    rc = zb_client_request(client, "DELETE", path, NULL, headers, NULL,
                           out_status, err);
    /* The header string held the capability; do not leave it in freed memory. */
    zb_free_secret(header);
    zb_free(path);
    return rc;
}

void zb_meta_free_token_list(char **tokens, size_t count)
{
    size_t i;
    if (tokens == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        zb_free(tokens[i]);
    }
    zb_free(tokens);
}

zb_status zb_meta_exists(zb_client *client, const char *const *tokens,
                         size_t token_count, char ***out_existing,
                         size_t *out_count, zb_error *err)
{
    char **found = NULL;
    size_t found_count = 0;
    size_t found_capacity = 0;
    size_t offset = 0;
    zb_status rc = ZB_OK;

    *out_existing = NULL;
    *out_count = 0;

    while (offset < token_count) {
        size_t batch = token_count - offset;
        zb_json *body;
        zb_json *array;
        zb_json *response = NULL;
        const zb_json *existing;
        size_t n;
        size_t i;

        if (batch > ZB_EXISTS_BATCH_MAX) {
            batch = ZB_EXISTS_BATCH_MAX;
        }

        body = zb_json_new_object();
        array = zb_json_new_array();
        if (body == NULL || array == NULL) {
            zb_json_free(body);
            zb_json_free(array);
            rc = zb_error_nomem(err);
            goto fail;
        }
        for (i = 0; i < batch; i++) {
            if (zb_json_arr_add(array, zb_json_new_string(tokens[offset + i])) !=
                0) {
                zb_json_free(array);
                zb_json_free(body);
                rc = zb_error_nomem(err);
                goto fail;
            }
        }
        if (zb_json_obj_add(body, "tokens", array) != 0) {
            zb_json_free(body);
            rc = zb_error_nomem(err);
            goto fail;
        }

        rc = zb_client_request(client, "POST", "/v1/uploads/exists", body, NULL,
                               &response, NULL, err);
        zb_json_free(body);
        if (rc != ZB_OK) {
            goto fail;
        }

        existing = zb_json_get(response, "existing");
        n = zb_json_array_len(existing);
        for (i = 0; i < n; i++) {
            const char *s = zb_json_str(zb_json_at(existing, i));
            char *copy;
            if (s == NULL) {
                continue;
            }
            if (found_count == found_capacity) {
                size_t cap = found_capacity != 0 ? found_capacity * 2 : 32;
                char **grown;
                if (cap > (size_t)-1 / sizeof(*grown)) {
                    zb_json_free(response);
                    rc = zb_error_nomem(err);
                    goto fail;
                }
                grown = zb_realloc(found, cap * sizeof(*grown));
                if (grown == NULL) {
                    zb_json_free(response);
                    rc = zb_error_nomem(err);
                    goto fail;
                }
                found = grown;
                found_capacity = cap;
            }
            copy = zb_strdup(s);
            if (copy == NULL) {
                zb_json_free(response);
                rc = zb_error_nomem(err);
                goto fail;
            }
            found[found_count++] = copy;
        }
        zb_json_free(response);
        offset += batch;
    }

    *out_existing = found;
    *out_count = found_count;
    return ZB_OK;

fail:
    /* Hand back nothing at all on failure: a partial answer would look like
     * "these tokens are gone" and cost the user their delete tokens. */
    zb_meta_free_token_list(found, found_count);
    return rc;
}

zb_status zb_meta_report(zb_client *client, const char *link, const char *note,
                         zb_json **out, zb_error *err)
{
    zb_json *body;
    zb_status rc;

    if (note != NULL && zb_utf8_len(note) > ZB_MAX_REPORT_NOTE_CHARS) {
        return zb_error_setf(
            err, ZB_ERR_USAGE,
            "the note is %llu characters; the limit is %d. Shorten it rather "
            "than letting it be cut off mid-sentence.",
            (unsigned long long)zb_utf8_len(note), ZB_MAX_REPORT_NOTE_CHARS);
    }

    body = zb_json_new_object();
    if (body == NULL) {
        return zb_error_nomem(err);
    }
    if (zb_json_obj_set_str(body, "link", link) != 0 ||
        (note != NULL && note[0] != '\0' &&
         zb_json_obj_set_str(body, "note", note) != 0)) {
        zb_json_free(body);
        return zb_error_nomem(err);
    }
    rc = zb_client_request(client, "POST", "/v1/reports", body, NULL, out, NULL,
                           err);
    zb_json_free(body);
    return rc;
}
