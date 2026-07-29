#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "api/meta.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/table.h"
#include "format/time.h"
#include "store/shelf.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/str.h"

void zb_help_ls(FILE *out)
{
    fputs(
        "Lists the uploads this machine knows about.\n"
        "\n"
        "There is no account behind zhuzhbox, so this list is local: it lives\n"
        "in shelf.json next to your config and it is the only record that a\n"
        "given upload is yours. Before listing, the tokens are checked against\n"
        "the server in batches and anything that has expired or been deleted\n"
        "is dropped. If that check fails for any reason the shelf is left\n"
        "exactly as it was — a failed network call is not evidence that\n"
        "anything expired.\n"
        "\n"
        "Options:\n"
        "      --sort KEY       newest (default), name, size or expires\n"
        "      --no-prune       skip the liveness check\n"
        "      --reveal-tokens  include delete tokens in --json output\n"
        "\n"
        "Delete tokens are never shown by default. They are capability\n"
        "secrets: anyone holding one can delete the upload.\n",
        out);
}

typedef struct {
    zb_shelf_sort sort;
    int no_prune;
    int reveal;
} ls_args;

static const zb_opt_spec k_specs[] = {
    {"sort", 0, ZB_ARG_REQUIRED, "KEY", "newest, name, size or expires"},
    {"no-prune", 0, ZB_ARG_NONE, NULL, "skip the liveness check"},
    {"reveal-tokens", 0, ZB_ARG_NONE, NULL,
     "include delete tokens in --json output"},
    {NULL, 0, ZB_ARG_NONE, NULL, NULL},
};

static zb_status on_option(void *ctx, const zb_opt_spec *spec,
                           const char *value, zb_error *err)
{
    ls_args *args = ctx;

    if (strcmp(spec->long_name, "sort") == 0) {
        if (zb_streq_ci(value, "newest") || zb_streq_ci(value, "uploaded")) {
            args->sort = ZB_SHELF_SORT_NEWEST;
        } else if (zb_streq_ci(value, "name")) {
            args->sort = ZB_SHELF_SORT_NAME;
        } else if (zb_streq_ci(value, "size")) {
            args->sort = ZB_SHELF_SORT_SIZE;
        } else if (zb_streq_ci(value, "expires")) {
            args->sort = ZB_SHELF_SORT_EXPIRES;
        } else {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "--sort takes newest, name, size or expires, "
                                 "not \"%s\"",
                                 value);
        }
    } else if (strcmp(spec->long_name, "no-prune") == 0) {
        args->no_prune = 1;
    } else if (strcmp(spec->long_name, "reveal-tokens") == 0) {
        args->reveal = 1;
    }
    return ZB_OK;
}

/* Mirrors the website's ZZ.shelf.prune(): ask which tokens are still live and
 * drop the rest. The whole new shelf is built in memory and written once. */
static void prune_shelf(zb_client *client, zb_options *opt, zb_shelf *shelf)
{
    const char **tokens = NULL;
    char **existing = NULL;
    size_t existing_count = 0;
    zb_shelf pruned;
    zb_error err;
    size_t i;
    size_t k;

    if (shelf->count == 0) {
        return;
    }
    zb_error_init(&err);
    zb_shelf_init(&pruned);

    tokens = zb_calloc(shelf->count, sizeof(*tokens));
    if (tokens == NULL) {
        return;
    }
    for (i = 0; i < shelf->count; i++) {
        tokens[i] = shelf->entries[i].token;
    }

    if (zb_meta_exists(client, tokens, shelf->count, &existing, &existing_count,
                       &err) != ZB_OK) {
        /* Deliberately silent about changing anything, because nothing
         * changed. The warning explains why the list may be stale. */
        if (!opt->quiet && !opt->json) {
            fprintf(stderr,
                    "zhuzhbox: could not check which uploads are still live "
                    "(%s); showing the local list unchanged\n",
                    zb_error_message(&err));
        }
        goto cleanup;
    }

    for (i = 0; i < shelf->count; i++) {
        int found = 0;
        for (k = 0; k < existing_count; k++) {
            if (existing[k] != NULL &&
                strcmp(existing[k], shelf->entries[i].token) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            continue;
        }
        if (zb_shelf_upsert(&pruned, &shelf->entries[i], &err) != ZB_OK) {
            zb_shelf_free(&pruned);
            goto cleanup;
        }
    }

    if (pruned.count != shelf->count) {
        if (zb_shelf_save(&pruned, opt->base_dir, &err) != ZB_OK) {
            if (!opt->quiet && !opt->json) {
                fprintf(stderr, "zhuzhbox: could not update the local list: %s\n",
                        zb_error_message(&err));
            }
            zb_shelf_free(&pruned);
            goto cleanup;
        }
        zb_shelf_free(shelf);
        *shelf = pruned;
        zb_shelf_init(&pruned);
    }
    zb_shelf_free(&pruned);

cleanup:
    zb_meta_free_token_list(existing, existing_count);
    zb_free((void *)(uintptr_t)tokens);
    zb_error_clear(&err);
}

static int print_table(const zb_options *opt, const zb_shelf *shelf)
{
    zb_table *table;
    int64_t now = zb_now_unix();
    size_t i;

    if (shelf->count == 0) {
        printf("%sNothing here yet. `zhuzhbox upload <file>` puts something on "
               "the shelf.%s\n",
               zb_color(opt->color, ZB_C_DIM), zb_color(opt->color, ZB_C_RESET));
        return ZB_EXIT_OK;
    }

    table = zb_table_new(5);
    if (table == NULL) {
        return ZB_EXIT_ERROR;
    }
    (void)zb_table_header(table, 0, "NAME");
    (void)zb_table_header(table, 1, "KIND");
    (void)zb_table_header(table, 2, "SIZE");
    (void)zb_table_header(table, 3, "UPLOADED");
    (void)zb_table_header(table, 4, "EXPIRES");
    (void)zb_table_align_right(table, 2);

    for (i = 0; i < shelf->count; i++) {
        const zb_shelf_entry *entry = &shelf->entries[i];
        char size_text[ZB_BYTES_BUF];
        char uploaded[ZB_TIME_BUF];
        char expires[ZB_TIME_BUF];
        int64_t expires_at = 0;
        int have_expiry = 0;

        zb_format_bytes(entry->size, size_text, sizeof(size_text));

        (void)zb_snprintf(uploaded, sizeof(uploaded), "%s", "unknown");
        if (entry->uploaded_at != NULL) {
            int64_t epoch = 0;
            if (zb_time_parse_iso8601(entry->uploaded_at, &epoch) == 0) {
                zb_time_relative(epoch, now, uploaded, sizeof(uploaded));
            }
        }
        (void)zb_snprintf(expires, sizeof(expires), "%s", "unknown");
        if (entry->expires_at != NULL &&
            zb_time_parse_iso8601(entry->expires_at, &expires_at) == 0) {
            zb_time_relative(expires_at, now, expires, sizeof(expires));
            have_expiry = 1;
        }

        if (zb_table_row(table) != 0) {
            zb_table_free(table);
            return ZB_EXIT_ERROR;
        }
        (void)zb_table_set(table, 0,
                           entry->name != NULL ? entry->name : entry->token);
        (void)zb_table_set(table, 1,
                           entry->kind != NULL ? entry->kind : "file");
        (void)zb_table_set(table, 2, size_text);
        (void)zb_table_set(table, 3, uploaded);
        (void)zb_table_set(table, 4, expires);

        /* Anything with under a day left is worth noticing before it goes. */
        if (have_expiry) {
            int64_t left = zb_time_until(expires_at, now);
            if (left == 0) {
                (void)zb_table_set_style(table, 4, zb_color(1, ZB_C_RED));
            } else if (left < 86400) {
                (void)zb_table_set_style(table, 4, zb_color(1, ZB_C_YELLOW));
            }
        }
    }

    zb_table_print(table, stdout, opt->color);
    zb_table_free(table);
    return ZB_EXIT_OK;
}

int zb_cmd_ls(zb_options *opt, int argc, char **argv)
{
    ls_args args;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    zb_client *client = NULL;
    zb_shelf shelf;
    int rc = ZB_EXIT_OK;

    memset(&args, 0, sizeof(args));
    args.sort = ZB_SHELF_SORT_NEWEST;
    zb_error_init(&err);
    zb_shelf_init(&shelf);

    if (zb_parse_args(argc, argv, k_specs, on_option, &args, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count > 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "ls takes no arguments");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (zb_shelf_load(&shelf, opt->base_dir, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (!args.no_prune && shelf.count > 0) {
        client = zb_client_new(opt, &err);
        if (client != NULL) {
            prune_shelf(client, opt, &shelf);
        } else {
            zb_error_clear(&err);
        }
    }

    zb_shelf_sort_entries(&shelf, args.sort);

    if (opt->json) {
        zb_json *array = zb_json_new_array();
        size_t i;
        if (array == NULL) {
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        for (i = 0; i < shelf.count; i++) {
            zb_json *item = zb_shelf_entry_json(&shelf.entries[i], args.reveal);
            if (item == NULL || zb_json_arr_add(array, item) != 0) {
                zb_json_free(array);
                zb_error_nomem(&err);
                rc = zb_report_error(opt, &err);
                goto cleanup;
            }
        }
        rc = zb_print_json(array);
        zb_json_free(array);
    } else {
        rc = print_table(opt, &shelf);
    }

cleanup:
    zb_shelf_free(&shelf);
    zb_client_free(client);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
