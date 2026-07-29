#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "api/meta.h"
#include "commands/commands.h"
#include "format/color.h"
#include "store/shelf.h"
#include "util/buf.h"
#include "util/str.h"
#include "util/token.h"

void zb_help_rm(FILE *out)
{
    fputs(
        "Deletes an upload from the server.\n"
        "\n"
        "If the token is on your local shelf its delete token is used\n"
        "automatically. Otherwise pass --delete-token, which is the\n"
        "equivalent of the site's \"by delete token\" tab — the server has no\n"
        "other way to know you are allowed to delete it.\n"
        "\n"
        "Deleting a collection cascades to every file in it.\n"
        "\n"
        "Options:\n"
        "  -t, --delete-token TOKEN  the delete token, if it is not local\n"
        "  -y, --yes                 do not ask for confirmation\n"
        "\n"
        "An upload that is already gone is reported as success: the outcome\n"
        "you asked for is the outcome you got.\n",
        out);
}

typedef struct {
    char *delete_token;
    int yes;
} rm_args;

static const zb_opt_spec k_specs[] = {
    {"delete-token", 't', ZB_ARG_REQUIRED, "TOKEN", "the delete token"},
    {"yes", 'y', ZB_ARG_NONE, NULL, "do not ask for confirmation"},
    {NULL, 0, ZB_ARG_NONE, NULL, NULL},
};

static zb_status on_option(void *ctx, const zb_opt_spec *spec,
                           const char *value, zb_error *err)
{
    rm_args *args = ctx;

    if (strcmp(spec->long_name, "delete-token") == 0) {
        char *copy = zb_strdup(value);
        if (copy == NULL) {
            return zb_error_nomem(err);
        }
        zb_free_secret(args->delete_token);
        args->delete_token = copy;
    } else if (strcmp(spec->long_name, "yes") == 0) {
        args->yes = 1;
    }
    return ZB_OK;
}

int zb_cmd_rm(zb_options *opt, int argc, char **argv)
{
    rm_args args;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    zb_client *client = NULL;
    zb_shelf shelf;
    zb_shelf_entry *entry = NULL;
    char *token = NULL;
    char *delete_token = NULL;
    long http_status = 0;
    int shelf_loaded = 0;
    int rc = ZB_EXIT_OK;

    memset(&args, 0, sizeof(args));
    zb_error_init(&err);
    zb_shelf_init(&shelf);

    if (zb_parse_args(argc, argv, k_specs, on_option, &args, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count != 1) {
        zb_error_setf(&err, ZB_ERR_USAGE, "rm takes exactly one link or token");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    token = zb_extract_token(positionals[0]);
    if (token == NULL) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "\"%s\" does not contain a zhuzhbox token",
                      positionals[0]);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    /* A broken shelf must not stop a delete the user can perform anyway with
     * an explicit --delete-token. */
    if (zb_shelf_load(&shelf, opt->base_dir, &err) == ZB_OK) {
        shelf_loaded = 1;
        entry = zb_shelf_find(&shelf, token);
    } else if (args.delete_token == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    } else {
        zb_error_clear(&err);
    }

    if (args.delete_token != NULL) {
        delete_token = zb_strdup(args.delete_token);
    } else if (entry != NULL && entry->delete_token != NULL) {
        delete_token = zb_strdup(entry->delete_token);
    } else {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "%s is not on this machine's shelf, so its delete token "
                      "is not known here — pass --delete-token",
                      token);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (delete_token == NULL) {
        zb_error_nomem(&err);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    {
        const char *label = entry != NULL && entry->name != NULL ? entry->name
                                                                 : token;
        int answer = zb_confirm(opt, args.yes, &err,
                                "Delete %s? This cannot be undone.", label);
        if (answer < 0) {
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        if (answer == 0) {
            zb_info(opt, "nothing deleted");
            goto cleanup;
        }
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (zb_meta_delete(client, token, delete_token, &http_status, &err) !=
        ZB_OK) {
        if (http_status == 404) {
            /* Already gone. Drop the local row and call it a success — that is
             * the state the user asked for. */
            zb_error_clear(&err);
            if (shelf_loaded && zb_shelf_remove(&shelf, token)) {
                (void)zb_shelf_save(&shelf, opt->base_dir, &err);
                zb_error_clear(&err);
            }
            if (opt->json) {
                zb_json *out = zb_json_new_object();
                if (out != NULL) {
                    (void)zb_json_obj_set_bool(out, "ok", 1);
                    (void)zb_json_obj_set_str(out, "token", token);
                    (void)zb_json_obj_set_bool(out, "alreadyGone", 1);
                    rc = zb_print_json(out);
                    zb_json_free(out);
                }
            } else {
                printf("%s was already gone.\n", token);
            }
            goto cleanup;
        }
        if (http_status == 403) {
            /* Keep the shelf row: the token we hold no longer matches, but the
             * upload is still there and the row is still the only record. */
            zb_error_setf(&err, ZB_ERR_HTTP,
                          "%s — the delete token no longer matches this "
                          "upload, so the local record was kept",
                          zb_error_message(&err));
        }
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (shelf_loaded && zb_shelf_remove(&shelf, token)) {
        if (zb_shelf_save(&shelf, opt->base_dir, &err) != ZB_OK) {
            fprintf(stderr,
                    "zhuzhbox: deleted on the server, but the local list could "
                    "not be updated: %s\n",
                    zb_error_message(&err));
            zb_error_clear(&err);
        }
    }

    if (opt->json) {
        zb_json *out = zb_json_new_object();
        if (out == NULL) {
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        (void)zb_json_obj_set_bool(out, "ok", 1);
        (void)zb_json_obj_set_str(out, "token", token);
        (void)zb_json_obj_set_bool(out, "alreadyGone", 0);
        rc = zb_print_json(out);
        zb_json_free(out);
    } else {
        printf("%sDeleted%s %s\n", zb_color(opt->color, ZB_C_GREEN),
               zb_color(opt->color, ZB_C_RESET), token);
    }

cleanup:
    zb_free_secret(delete_token);
    zb_free_secret(args.delete_token);
    zb_free(token);
    zb_shelf_free(&shelf);
    zb_client_free(client);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
