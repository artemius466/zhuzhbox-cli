/* collection.h — bundles: several files behind one link.
 *
 * Member files inherit the collection's expiry rather than their own
 * size-based tier, so a bundle never loses one member while the rest live on.
 */
#ifndef ZB_API_COLLECTION_H
#define ZB_API_COLLECTION_H

#include "api/client.h"
#include "util/error.h"
#include "util/json.h"

/* Title and description are optional; both are validated client-side against
 * the server's limits in UTF-8 code points, not bytes — a 200-character
 * Cyrillic title is 400 bytes and must not be rejected for that. */
zb_status zb_collection_validate(const char *title, const char *description,
                                 zb_error *err);

/* POST /v1/upload/collection/init -> { token, deleteToken, expiresAt } */
zb_status zb_collection_init(zb_client *client, const char *title,
                             const char *description, zb_json **out,
                             zb_error *err);

/* POST /v1/upload/collection/{token}/complete. Seals the collection: further
 * members are rejected with 409 afterwards. */
zb_status zb_collection_complete(zb_client *client, const char *token,
                                 zb_json **out, zb_error *err);

#endif /* ZB_API_COLLECTION_H */
