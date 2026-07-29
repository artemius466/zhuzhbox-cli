#include <stdio.h>

#include "api/client.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "util/buf.h"

void zb_help_quota(FILE *out)
{
    fputs("Shows GET /v1/quota: how much of your rolling upload allowance is\n"
          "left. There is no account behind this — the server keys it to a\n"
          "salted hash of your IP address, and it counts only uploads that are\n"
          "still live.\n"
          "\n"
          "Exhausted quota is information, not a failure: this command exits 0\n"
          "unless the request itself could not be made.\n",
          out);
}

/* A 30-cell bar is wide enough to read at a glance and narrow enough to fit
 * beside the numbers on an 80-column terminal. */
static void print_bar(uint64_t used, uint64_t limit, int color)
{
    const int width = 30;
    int filled = 0;
    int i;
    const char *shade;

    if (limit > 0) {
        double ratio = (double)used / (double)limit;
        if (ratio > 1.0) {
            ratio = 1.0;
        }
        filled = (int)(ratio * width + 0.5);
    }
    shade = filled * 10 >= width * 9 ? zb_color(color, ZB_C_RED)
            : filled * 10 >= width * 7 ? zb_color(color, ZB_C_YELLOW)
                                       : zb_color(color, ZB_C_GREEN);

    fputs("  [", stdout);
    fputs(shade, stdout);
    for (i = 0; i < width; i++) {
        if (i == filled) {
            fputs(zb_color(color, ZB_C_RESET), stdout);
            fputs(zb_color(color, ZB_C_DIM), stdout);
        }
        fputc(i < filled ? '#' : '.', stdout);
    }
    fputs(zb_color(color, ZB_C_RESET), stdout);
    fputs("]\n", stdout);
}

int zb_cmd_quota(zb_options *opt, int argc, char **argv)
{
    zb_client *client = NULL;
    zb_json *response = NULL;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    uint64_t used = 0;
    uint64_t limit = 0;
    uint64_t remaining = 0;
    int64_t window_days = 0;
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count > 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "quota takes no arguments");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (zb_client_request(client, "GET", "/v1/quota", NULL, NULL, &response,
                          NULL, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (opt->json) {
        rc = zb_print_json(response);
        goto cleanup;
    }

    if (!zb_json_get_u64(response, "usedBytes", &used) ||
        !zb_json_get_u64(response, "limitBytes", &limit)) {
        zb_error_setf(&err, ZB_ERR_PROTO,
                      "the quota response did not contain usable numbers");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    (void)zb_json_get_u64(response, "remainingBytes", &remaining);
    (void)zb_json_get_i64(response, "windowDays", &window_days);

    {
        char used_text[ZB_BYTES_BUF];
        char limit_text[ZB_BYTES_BUF];
        char remaining_text[ZB_BYTES_BUF];
        const char *bold = zb_color(opt->color, ZB_C_BOLD);
        const char *dim = zb_color(opt->color, ZB_C_DIM);
        const char *reset = zb_color(opt->color, ZB_C_RESET);

        zb_format_bytes(used, used_text, sizeof(used_text));
        zb_format_bytes(limit, limit_text, sizeof(limit_text));
        zb_format_bytes(remaining, remaining_text, sizeof(remaining_text));

        printf("%sUsed%s      %s %s(%llu bytes)%s\n", bold, reset, used_text,
               dim, (unsigned long long)used, reset);
        printf("%sLimit%s     %s %s(%llu bytes)%s\n", bold, reset, limit_text,
               dim, (unsigned long long)limit, reset);
        printf("%sRemaining%s %s %s(%llu bytes)%s\n", bold, reset,
               remaining_text, dim, (unsigned long long)remaining, reset);
        print_bar(used, limit, opt->color);
        if (window_days > 0) {
            printf("%sRolling %lld-day window; only uploads that are still "
                   "live count.%s\n",
                   dim, (long long)window_days, reset);
        }
    }

cleanup:
    zb_free(positionals);
    zb_json_free(response);
    zb_client_free(client);
    zb_error_clear(&err);
    return rc;
}
