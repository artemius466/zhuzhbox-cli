#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/table.h"
#include "format/time.h"
#include "util/buf.h"
#include "util/platform.h"

void zb_help_stats(FILE *out)
{
    fputs("Shows GET /v1/stats: service-wide totals, the server's disk usage\n"
          "if it reports any, and the recent uploads-per-day series.\n"
          "\n"
          "These are numbers for the whole service, not for you — `zhuzhbox\n"
          "quota` is the one about your own usage.\n",
          out);
}

static void print_disk(const zb_options *opt, const zb_json *stats)
{
    const zb_json *disk = zb_json_get(stats, "disk");
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    char total_text[ZB_BYTES_BUF];
    char free_text[ZB_BYTES_BUF];
    const char *dim = zb_color(opt->color, ZB_C_DIM);
    const char *reset = zb_color(opt->color, ZB_C_RESET);

    /* `disk` may be JSON null, or absent entirely. Both mean "not reported". */
    if (disk == NULL || zb_json_is_null(disk) || !zb_json_is_object(disk)) {
        printf("Disk        %snot reported%s\n", dim, reset);
        return;
    }
    if (!zb_json_get_u64(disk, "totalBytes", &total) ||
        !zb_json_get_u64(disk, "freeBytes", &free_bytes)) {
        printf("Disk        %snot reported%s\n", dim, reset);
        return;
    }
    zb_format_bytes(total, total_text, sizeof(total_text));
    zb_format_bytes(free_bytes, free_text, sizeof(free_text));
    printf("Disk        %s free of %s\n", free_text, total_text);
}

static void print_by_day(const zb_options *opt, const zb_json *stats)
{
    const zb_json *series = zb_json_get(stats, "uploadsByDay");
    size_t count = zb_json_array_len(series);
    size_t i;
    zb_table *table;

    if (count == 0) {
        return;
    }
    printf("\n%sUploads by day%s\n", zb_color(opt->color, ZB_C_BOLD),
           zb_color(opt->color, ZB_C_RESET));

    table = zb_table_new(3);
    if (table == NULL) {
        return;
    }
    (void)zb_table_header(table, 0, "DATE");
    (void)zb_table_header(table, 1, "FILES");
    (void)zb_table_header(table, 2, "BYTES");
    (void)zb_table_align_right(table, 1);
    (void)zb_table_align_right(table, 2);

    for (i = 0; i < count; i++) {
        const zb_json *day = zb_json_at(series, i);
        const char *date = NULL;
        uint64_t files = 0;
        uint64_t bytes = 0;
        char bytes_text[ZB_BYTES_BUF];

        (void)zb_json_get_str(day, "date", &date);
        (void)zb_json_get_u64(day, "files", &files);
        (void)zb_json_get_u64(day, "bytes", &bytes);
        zb_format_bytes(bytes, bytes_text, sizeof(bytes_text));

        if (zb_table_row(table) != 0) {
            break;
        }
        (void)zb_table_set(table, 0, date != NULL ? date : "?");
        (void)zb_table_setf(table, 1, "%llu", (unsigned long long)files);
        (void)zb_table_set(table, 2, bytes_text);
    }
    zb_table_print(table, stdout, opt->color);
    zb_table_free(table);
}

int zb_cmd_stats(zb_options *opt, int argc, char **argv)
{
    zb_client *client = NULL;
    zb_json *stats = NULL;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count > 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "stats takes no arguments");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (zb_client_request(client, "GET", "/v1/stats", NULL, NULL, &stats, NULL,
                          &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (opt->json) {
        rc = zb_print_json(stats);
        goto cleanup;
    }

    {
        uint64_t total_bytes = 0;
        uint64_t total_files = 0;
        uint64_t total_collections = 0;
        const char *generated = NULL;
        char bytes_text[ZB_BYTES_BUF];
        const char *dim = zb_color(opt->color, ZB_C_DIM);
        const char *reset = zb_color(opt->color, ZB_C_RESET);

        (void)zb_json_get_u64(stats, "totalBytes", &total_bytes);
        (void)zb_json_get_u64(stats, "totalFiles", &total_files);
        (void)zb_json_get_u64(stats, "totalCollections", &total_collections);
        (void)zb_json_get_str(stats, "generatedAt", &generated);
        zb_format_bytes(total_bytes, bytes_text, sizeof(bytes_text));

        printf("Files       %llu\n", (unsigned long long)total_files);
        printf("Collections %llu\n", (unsigned long long)total_collections);
        printf("Stored      %s %s(%llu bytes)%s\n", bytes_text, dim,
               (unsigned long long)total_bytes, reset);
        print_disk(opt, stats);

        if (generated != NULL) {
            int64_t epoch = 0;
            if (zb_time_parse_iso8601(generated, &epoch) == 0) {
                char relative[ZB_TIME_BUF];
                zb_time_relative(epoch, zb_now_unix(), relative,
                                 sizeof(relative));
                printf("%sGenerated %s%s\n", dim, relative, reset);
            }
        }
        print_by_day(opt, stats);
    }

cleanup:
    zb_json_free(stats);
    zb_client_free(client);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
