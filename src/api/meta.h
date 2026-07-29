/* meta.h — everything that is not an upload: metadata, deletion, liveness
 * checks, and abuse reports. */
#ifndef ZB_API_META_H
#define ZB_API_META_H

#include <stddef.h>

#include "api/client.h"
#include "util/error.h"
#include "util/json.h"

/* GET /v1/upload/{token}. The result is either a file record or a collection
 * record; check its "type" field. 404 is surfaced as the server's own
 * "Not found or expired." string. */
zb_status zb_meta_get(zb_client *client, const char *token, zb_json **out,
                      zb_error *err);

/* DELETE /v1/upload/{token} with X-Delete-Token.
 *
 * *out_status receives the HTTP status so the caller can tell 404 (already
 * gone — treated as success everywhere in this program) from 403 (the token no
 * longer matches, so the shelf entry should be kept). */
zb_status zb_meta_delete(zb_client *client, const char *token,
                         const char *delete_token, long *out_status,
                         zb_error *err);

/* POST /v1/uploads/exists, batched at the server's 500-token limit.
 *
 * On success *out_existing is an owned array of owned strings (free each, then
 * the array). On ANY failure the caller must leave local state alone — a
 * failed liveness check is not evidence that anything expired. */
zb_status zb_meta_exists(zb_client *client, const char *const *tokens,
                         size_t token_count, char ***out_existing,
                         size_t *out_count, zb_error *err);

void zb_meta_free_token_list(char **tokens, size_t count);

/* POST /v1/reports. `note` may be NULL; it is capped client-side at 2000 UTF-8
 * code points with a clear error rather than being silently truncated. */
zb_status zb_meta_report(zb_client *client, const char *link, const char *note,
                         zb_json **out, zb_error *err);

#endif /* ZB_API_META_H */
