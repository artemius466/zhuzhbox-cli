#include "store/atomic.h"

#include <stdio.h>
#include <string.h>

#include "util/buf.h"
#include "util/platform.h"

zb_status zb_atomic_write(const char *path, const char *data, size_t len,
                          zb_error *err)
{
    char *dir = NULL;
    char *tmp = NULL;
    FILE *fp = NULL;
    unsigned attempt;
    zb_status rc = ZB_OK;

    dir = zb_path_dirname(path);
    if (dir == NULL) {
        return zb_error_nomem(err);
    }
    if (zb_mkdir_p(dir) != 0) {
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot create directory %s", dir);
        goto cleanup;
    }

    /* The temp file must live in the same directory as the target, or the
     * rename would cross filesystems and stop being atomic. */
    for (attempt = 0; attempt < 32; attempt++) {
        char suffix[64];
        (void)snprintf(suffix, sizeof(suffix), "%llx-%u.tmp",
                       (unsigned long long)zb_now_ms(), attempt);
        zb_free(tmp);
        tmp = zb_asprintf("%s.%s", path, suffix);
        if (tmp == NULL) {
            rc = zb_error_nomem(err);
            goto cleanup;
        }
        fp = zb_fopen_private_new(tmp);
        if (fp != NULL) {
            break;
        }
    }
    if (fp == NULL) {
        rc = zb_error_setf(err, ZB_ERR_IO,
                           "cannot create a temporary file next to %s", path);
        goto cleanup;
    }

    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot write %s", tmp);
        goto cleanup;
    }
    if (zb_fsync(fp) != 0) {
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot flush %s to disk", tmp);
        goto cleanup;
    }
    if (fclose(fp) != 0) {
        fp = NULL;
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot close %s", tmp);
        goto cleanup;
    }
    fp = NULL;

    if (zb_rename_replace(tmp, path) != 0) {
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot replace %s", path);
        goto cleanup;
    }
    zb_free(tmp);
    tmp = NULL;

cleanup:
    if (fp != NULL) {
        fclose(fp);
    }
    if (tmp != NULL) {
        zb_remove(tmp);
        zb_free(tmp);
    }
    zb_free(dir);
    return rc;
}

zb_status zb_read_file(const char *path, char **out, size_t *out_len,
                       zb_error *err)
{
    FILE *fp;
    zb_buf buf;
    char chunk[8192];
    size_t n;

    *out = NULL;
    *out_len = 0;

    fp = zb_fopen(path, "rb");
    if (fp == NULL) {
        /* Absent is not an error: callers treat it as "empty". */
        return ZB_OK;
    }

    zb_buf_init(&buf);
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (zb_buf_append(&buf, chunk, n) != 0) {
            zb_buf_free(&buf);
            fclose(fp);
            return zb_error_nomem(err);
        }
    }
    if (ferror(fp)) {
        zb_buf_free(&buf);
        fclose(fp);
        return zb_error_setf(err, ZB_ERR_IO, "cannot read %s", path);
    }
    fclose(fp);

    *out_len = buf.len;
    *out = zb_buf_detach(&buf);
    zb_buf_free(&buf);
    if (*out == NULL) {
        return zb_error_nomem(err);
    }
    return ZB_OK;
}
