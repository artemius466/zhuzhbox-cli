#include <stdio.h>
#include <string.h>

#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/time.h"
#include "store/atomic.h"
#include "store/paths.h"
#include "store/shelf.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/str.h"
#include "util/token.h"

void zb_help_shelf(FILE *out)
{
    fputs(
        "Backs up and restores the local uploads list.\n"
        "\n"
        "  zhuzhbox shelf export PATH   write shelf.json to PATH\n"
        "  zhuzhbox shelf import PATH   merge PATH into the local shelf\n"
        "  zhuzhbox shelf show TOKEN    show one entry, delete token included\n"
        "\n"
        "This is the only backup story for delete capability. Uploads are\n"
        "encrypted at rest with keys derived from the token in the share link,\n"
        "so the server cannot tell you which uploads are yours and cannot\n"
        "reissue a delete token. Lose shelf.json without a copy and you lose\n"
        "the ability to delete anything whose delete token you did not write\n"
        "down separately.\n"
        "\n"
        "The exported file contains every delete token in plain text. It is\n"
        "created readable only by you (mode 0600 on Linux and macOS; on\n"
        "Windows it inherits your profile's ACL) but it is not encrypted on\n"
        "any platform. Treat it like a password file.\n"
        "\n"
        "On import, entries in the file win over local ones with the same\n"
        "token.\n",
        out);
}

static int shelf_export(zb_options *opt, const char *path, zb_error *err)
{
    zb_shelf shelf;
    char *source = NULL;
    char *text = NULL;
    size_t len = 0;
    int rc = ZB_EXIT_OK;

    zb_shelf_init(&shelf);

    /* Load first, so a corrupt shelf is reported rather than copied. */
    if (zb_shelf_load(&shelf, opt->base_dir, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    if (shelf.count == 0) {
        zb_error_setf(err, ZB_ERR_IO, "there is nothing on the shelf to export");
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    fprintf(stderr,
            "zhuzhbox: %s will contain %llu delete token%s in plain text. It is "
            "created readable only by you, but it is not encrypted.\n",
            path, (unsigned long long)shelf.count, shelf.count == 1 ? "" : "s");

    source = zb_paths_file(opt->base_dir, ZB_SHELF_FILE_NAME);
    if (source == NULL) {
        zb_error_nomem(err);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    /* Copy the file verbatim rather than re-serializing: what you back up is
     * exactly what you had. */
    if (zb_read_file(source, &text, &len, err) != ZB_OK || text == NULL) {
        if (err->status == ZB_OK) {
            zb_error_setf(err, ZB_ERR_IO, "%s does not exist yet", source);
        }
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    if (zb_atomic_write(path, text, len, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    if (opt->json) {
        zb_json *out = zb_json_new_object();
        if (out != NULL) {
            (void)zb_json_obj_set_bool(out, "ok", 1);
            (void)zb_json_obj_set_str(out, "path", path);
            (void)zb_json_obj_set_u64(out, "entries", shelf.count);
            rc = zb_print_json(out);
            zb_json_free(out);
        }
    } else {
        printf("Exported %llu entr%s to %s\n", (unsigned long long)shelf.count,
               shelf.count == 1 ? "y" : "ies", path);
    }

cleanup:
    if (text != NULL) {
        zb_free_secret(text);
    }
    zb_free(source);
    zb_shelf_free(&shelf);
    return rc;
}

static int shelf_import(zb_options *opt, const char *path, zb_error *err)
{
    zb_shelf local;
    zb_shelf incoming;
    char *dir = NULL;
    char *base = NULL;
    size_t i;
    size_t added = 0;
    int rc = ZB_EXIT_OK;

    zb_shelf_init(&local);
    zb_shelf_init(&incoming);

    /* zb_shelf_load reads <dir>/shelf.json, so point it at the file we were
     * given by splitting the path. */
    dir = zb_path_dirname(path);
    base = zb_path_basename(path);
    if (dir == NULL || base == NULL) {
        zb_error_nomem(err);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    if (strcmp(base, ZB_SHELF_FILE_NAME) != 0) {
        /* Read it directly instead of pretending it is a config directory. */
        char *text = NULL;
        size_t len = 0;
        zb_json *root = NULL;
        const zb_json *entries;
        size_t count;

        if (zb_read_file(path, &text, &len, err) != ZB_OK || text == NULL) {
            if (err->status == ZB_OK) {
                zb_error_setf(err, ZB_ERR_IO, "%s does not exist", path);
            }
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        root = zb_json_parse(text, len);
        zb_free_secret(text);
        if (root == NULL || !zb_json_is_object(root)) {
            zb_json_free(root);
            zb_error_setf(err, ZB_ERR_IO, "%s is not valid JSON", path);
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        entries = zb_json_get(root, "entries");
        count = zb_json_array_len(entries);
        for (i = 0; i < count; i++) {
            const zb_json *node = zb_json_at(entries, i);
            zb_shelf_entry entry;
            const char *s = NULL;

            memset(&entry, 0, sizeof(entry));
            if (!zb_json_get_str(node, "token", &s)) {
                continue;
            }
            entry.token = (char *)(uintptr_t)s;
            if (zb_json_get_str(node, "deleteToken", &s)) {
                entry.delete_token = (char *)(uintptr_t)s;
            }
            if (zb_json_get_str(node, "kind", &s)) {
                entry.kind = (char *)(uintptr_t)s;
            }
            if (zb_json_get_str(node, "name", &s)) {
                entry.name = (char *)(uintptr_t)s;
            }
            if (zb_json_get_str(node, "url", &s)) {
                entry.url = (char *)(uintptr_t)s;
            }
            if (zb_json_get_str(node, "uploadedAt", &s)) {
                entry.uploaded_at = (char *)(uintptr_t)s;
            }
            if (zb_json_get_str(node, "expiresAt", &s)) {
                entry.expires_at = (char *)(uintptr_t)s;
            }
            (void)zb_json_get_u64(node, "size", &entry.size);

            if (zb_shelf_upsert(&incoming, &entry, err) != ZB_OK) {
                zb_json_free(root);
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
        }
        zb_json_free(root);
    } else if (zb_shelf_load(&incoming, dir, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    if (incoming.count == 0) {
        zb_error_setf(err, ZB_ERR_IO, "%s has no entries in it", path);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    if (zb_shelf_load(&local, opt->base_dir, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    /* Imported entries win: they are the ones the user chose to restore. */
    for (i = 0; i < incoming.count; i++) {
        if (zb_shelf_find(&local, incoming.entries[i].token) == NULL) {
            added++;
        }
        if (zb_shelf_upsert(&local, &incoming.entries[i], err) != ZB_OK) {
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
    }
    if (zb_shelf_save(&local, opt->base_dir, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    if (opt->json) {
        zb_json *out = zb_json_new_object();
        if (out != NULL) {
            (void)zb_json_obj_set_bool(out, "ok", 1);
            (void)zb_json_obj_set_u64(out, "imported", incoming.count);
            (void)zb_json_obj_set_u64(out, "new", added);
            (void)zb_json_obj_set_u64(out, "total", local.count);
            rc = zb_print_json(out);
            zb_json_free(out);
        }
    } else {
        printf("Imported %llu entr%s (%llu new); the shelf now holds %llu.\n",
               (unsigned long long)incoming.count,
               incoming.count == 1 ? "y" : "ies", (unsigned long long)added,
               (unsigned long long)local.count);
    }

cleanup:
    zb_free(dir);
    zb_free(base);
    zb_shelf_free(&incoming);
    zb_shelf_free(&local);
    return rc;
}

static int shelf_show(zb_options *opt, const char *input, zb_error *err)
{
    zb_shelf shelf;
    zb_shelf_entry *entry;
    char *token = NULL;
    int rc = ZB_EXIT_OK;

    zb_shelf_init(&shelf);

    token = zb_extract_token(input);
    if (token == NULL) {
        zb_error_setf(err, ZB_ERR_USAGE,
                      "\"%s\" does not contain a zhuzhbox token", input);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    if (zb_shelf_load(&shelf, opt->base_dir, err) != ZB_OK) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    entry = zb_shelf_find(&shelf, token);
    if (entry == NULL) {
        zb_error_setf(err, ZB_ERR_IO, "%s is not on this machine's shelf",
                      token);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }

    if (opt->json) {
        /* This view exists precisely to reveal the delete token. */
        zb_json *out = zb_shelf_entry_json(entry, 1);
        if (out == NULL) {
            zb_error_nomem(err);
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        rc = zb_print_json(out);
        zb_json_free(out);
    } else {
        char size_text[ZB_BYTES_BUF];
        const char *dim = zb_color(opt->color, ZB_C_DIM);
        const char *yellow = zb_color(opt->color, ZB_C_YELLOW);
        const char *reset = zb_color(opt->color, ZB_C_RESET);

        zb_format_bytes(entry->size, size_text, sizeof(size_text));
        printf("name          %s\n", entry->name != NULL ? entry->name : "");
        printf("kind          %s\n", entry->kind != NULL ? entry->kind : "file");
        printf("token         %s\n", entry->token);
        printf("size          %s\n", size_text);
        printf("url           %s\n", entry->url != NULL ? entry->url : "");
        printf("uploaded      %s\n",
               entry->uploaded_at != NULL ? entry->uploaded_at : "");
        printf("expires       %s\n",
               entry->expires_at != NULL ? entry->expires_at : "");
        printf("delete token  %s%s%s\n", yellow,
               entry->delete_token != NULL ? entry->delete_token : "(none)",
               reset);
        printf("%sAnyone holding that delete token can delete this upload.%s\n",
               dim, reset);
    }

cleanup:
    zb_free(token);
    zb_shelf_free(&shelf);
    return rc;
}

int zb_cmd_shelf(zb_options *opt, int argc, char **argv)
{
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    const char *action;
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count != 2) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "shelf needs an action (export, import or show) and a "
                      "path or token");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    action = positionals[0];

    if (strcmp(action, "export") == 0) {
        rc = shelf_export(opt, positionals[1], &err);
    } else if (strcmp(action, "import") == 0) {
        rc = shelf_import(opt, positionals[1], &err);
    } else if (strcmp(action, "show") == 0) {
        rc = shelf_show(opt, positionals[1], &err);
    } else {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "shelf takes export, import or show, not \"%s\"", action);
        rc = zb_report_error(opt, &err);
    }

cleanup:
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
