/* atomic.h — the write discipline every file in the config directory uses.
 *
 * Write to a temp file in the same directory, fsync it, close it, rename it
 * over the target. A crash mid-write leaves the previous file intact; it never
 * leaves a truncated shelf holding half your delete tokens.
 *
 * The temp file is created O_EXCL with mode 0600 so the permissions are right
 * from the instant it exists — no create-then-chmod window, no umask
 * dependence. On Windows the rename goes through MoveFileExW, because plain
 * rename() there fails when the destination already exists.
 */
#ifndef ZB_STORE_ATOMIC_H
#define ZB_STORE_ATOMIC_H

#include <stddef.h>

#include "util/error.h"

/* Replace `path` with `data`. Creates the parent directory if missing. */
zb_status zb_atomic_write(const char *path, const char *data, size_t len,
                          zb_error *err);

/* Read a whole file into an owned NUL-terminated buffer.
 *
 * Returns ZB_OK with *out set to NULL and *out_len 0 when the file does not
 * exist — a missing shelf is an empty shelf, not an error. */
zb_status zb_read_file(const char *path, char **out, size_t *out_len,
                       zb_error *err);

#endif /* ZB_STORE_ATOMIC_H */
