#include "api/collection.h"

#include "zb_limits.h"
#include "util/buf.h"
#include "util/str.h"

zb_status zb_collection_validate(const char *title, const char *description,
                                 zb_error *err)
{
    if (title != NULL && zb_utf8_len(title) > ZB_MAX_TITLE_CHARS) {
        return zb_error_setf(err, ZB_ERR_USAGE,
                             "the title is %llu characters; the limit is %d",
                             (unsigned long long)zb_utf8_len(title),
                             ZB_MAX_TITLE_CHARS);
    }
    if (description != NULL &&
        zb_utf8_len(description) > ZB_MAX_DESCRIPTION_CHARS) {
        return zb_error_setf(
            err, ZB_ERR_USAGE,
            "the description is %llu characters; the limit is %d",
            (unsigned long long)zb_utf8_len(description),
            ZB_MAX_DESCRIPTION_CHARS);
    }
    return ZB_OK;
}

zb_status zb_collection_init(zb_client *client, const char *title,
                             const char *description, zb_json **out,
                             zb_error *err)
{
    zb_json *body;
    zb_status rc;

    rc = zb_collection_validate(title, description, err);
    if (rc != ZB_OK) {
        return rc;
    }

    body = zb_json_new_object();
    if (body == NULL) {
        return zb_error_nomem(err);
    }
    /* Both fields are optional; sending null is the same as omitting them. */
    if ((title != NULL && zb_json_obj_set_str(body, "title", title) != 0) ||
        (description != NULL &&
         zb_json_obj_set_str(body, "description", description) != 0)) {
        zb_json_free(body);
        return zb_error_nomem(err);
    }

    rc = zb_client_request(client, "POST", "/v1/upload/collection/init", body,
                           NULL, out, NULL, err);
    zb_json_free(body);
    return rc;
}

zb_status zb_collection_complete(zb_client *client, const char *token,
                                 zb_json **out, zb_error *err)
{
    char *path = zb_asprintf("/v1/upload/collection/%s/complete", token);
    zb_status rc;

    if (path == NULL) {
        return zb_error_nomem(err);
    }
    rc = zb_client_request(client, "POST", path, NULL, NULL, out, NULL, err);
    zb_free(path);
    return rc;
}
