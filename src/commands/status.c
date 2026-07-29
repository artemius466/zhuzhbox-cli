#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "api/meta.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/table.h"
#include "format/time.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/token.h"

void zb_help_status(FILE *out)
{
    fputs("Shows what a link points at — type, name, size and expiry — without\n"
          "downloading anything and without touching your local shelf. Useful\n"
          "for checking a link someone sent you, or for confirming that\n"
          "something you shared is still live.\n",
          out);
}

static void print_record(const zb_options *opt, const zb_json *record,
                         const char *token)
{
    const char *bold = zb_color(opt->color, ZB_C_BOLD);
    const char *dim = zb_color(opt->color, ZB_C_DIM);
    const char *reset = zb_color(opt->color, ZB_C_RESET);
    const char *type = NULL;
    const char *name = NULL;
    const char *mime = NULL;
    const char *uploaded = NULL;
    const char *expires = NULL;
    uint64_t size = 0;
    char size_text[ZB_BYTES_BUF];
    int64_t now = zb_now_unix();

    (void)zb_json_get_str(record, "type", &type);
    if (!zb_json_get_str(record, "filename", &name)) {
        (void)zb_json_get_str(record, "title", &name);
    }
    (void)zb_json_get_str(record, "mimeType", &mime);
    (void)zb_json_get_str(record, "uploadedAt", &uploaded);
    (void)zb_json_get_str(record, "expiresAt", &expires);
    if (!zb_json_get_u64(record, "size", &size)) {
        (void)zb_json_get_u64(record, "totalSize", &size);
    }
    zb_format_bytes(size, size_text, sizeof(size_text));

    printf("%s%s%s\n", bold,
           name != NULL && name[0] != '\0' ? name : token, reset);
    printf("  type      %s\n", type != NULL ? type : "file");
    printf("  token     %s\n", token);
    printf("  size      %s %s(%llu bytes)%s\n", size_text, dim,
           (unsigned long long)size, reset);
    if (mime != NULL) {
        printf("  mime      %s\n", mime);
    }

    {
        char stamp[ZB_TIME_BUF];
        char relative[ZB_TIME_BUF];
        int64_t epoch = 0;

        if (uploaded != NULL && zb_time_parse_iso8601(uploaded, &epoch) == 0) {
            zb_time_format_stamp(epoch, stamp, sizeof(stamp));
            zb_time_relative(epoch, now, relative, sizeof(relative));
            printf("  uploaded  %s %s(%s)%s\n", stamp, dim, relative, reset);
        }
        if (expires != NULL && zb_time_parse_iso8601(expires, &epoch) == 0) {
            const char *shade =
                zb_time_until(epoch, now) < 86400
                    ? zb_color(opt->color, ZB_C_YELLOW)
                    : "";
            zb_time_format_stamp(epoch, stamp, sizeof(stamp));
            zb_time_relative(epoch, now, relative, sizeof(relative));
            printf("  expires   %s%s%s %s(%s)%s\n", shade, stamp, reset, dim,
                   relative, reset);
        }
    }

    if (type != NULL && strcmp(type, "collection") == 0) {
        const zb_json *files = zb_json_get(record, "files");
        size_t count = zb_json_array_len(files);
        size_t i;
        zb_table *table;

        printf("\n%s%llu file%s in this bundle:%s\n", dim,
               (unsigned long long)count, count == 1 ? "" : "s", reset);
        table = zb_table_new(3);
        if (table == NULL) {
            return;
        }
        (void)zb_table_align_right(table, 1);
        for (i = 0; i < count; i++) {
            const zb_json *file = zb_json_at(files, i);
            const char *file_name = NULL;
            const char *file_token = NULL;
            uint64_t file_size = 0;
            char file_size_text[ZB_BYTES_BUF];

            (void)zb_json_get_str(file, "filename", &file_name);
            (void)zb_json_get_str(file, "token", &file_token);
            (void)zb_json_get_u64(file, "size", &file_size);
            zb_format_bytes(file_size, file_size_text, sizeof(file_size_text));

            if (zb_table_row(table) != 0) {
                break;
            }
            (void)zb_table_set(table, 0,
                               file_name != NULL ? file_name : "(unnamed)");
            (void)zb_table_set(table, 1, file_size_text);
            (void)zb_table_set(table, 2, file_token != NULL ? file_token : "");
        }
        zb_table_print(table, stdout, opt->color);
        zb_table_free(table);
    }
}

int zb_cmd_status(zb_options *opt, int argc, char **argv)
{
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    zb_client *client = NULL;
    zb_json *record = NULL;
    char *token = NULL;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count != 1) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "status takes exactly one link or token");
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

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (zb_meta_get(client, token, &record, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (opt->json) {
        rc = zb_print_json(record);
    } else {
        print_record(opt, record, token);
    }

cleanup:
    zb_free(token);
    zb_json_free(record);
    zb_client_free(client);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
