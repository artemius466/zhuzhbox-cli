/* mime.h — extension to MIME type, from a small built-in table.
 *
 * Deliberately not libmagic and deliberately not shelling out to file(1): the
 * server only uses this string as a hint, and a wrong guess costs nothing. */
#ifndef ZB_UTIL_MIME_H
#define ZB_UTIL_MIME_H

/* Never NULL — falls back to "application/octet-stream". The returned string
 * is static; do not free it. */
const char *zb_mime_from_path(const char *path);

/* A plausible extension for a MIME type (including the leading dot), or NULL
 * if there is no good guess. Static storage. */
const char *zb_ext_from_mime(const char *mime);

#endif /* ZB_UTIL_MIME_H */
