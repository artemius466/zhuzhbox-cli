#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/time.h"
#include "util/buf.h"

void zb_help_health(FILE *out)
{
    fputs("Checks GET /v1/health. Exits 0 only when the service reports\n"
          "status \"ok\", so it works as a scriptable healthcheck:\n"
          "\n"
          "  zhuzhbox health --json | jq -r .status\n",
          out);
}

int zb_cmd_health(zb_options *opt, int argc, char **argv)
{
    zb_client *client = NULL;
    zb_json *response = NULL;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    const char *status_text = NULL;
    int64_t uptime = 0;
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count > 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "health takes no arguments");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (zb_client_request(client, "GET", "/v1/health", NULL, NULL, &response,
                          NULL, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    (void)zb_json_get_str(response, "status", &status_text);
    (void)zb_json_get_i64(response, "uptimeSeconds", &uptime);

    if (opt->json) {
        rc = zb_print_json(response);
    } else {
        char uptime_text[ZB_TIME_BUF];
        const char *green = zb_color(opt->color, ZB_C_GREEN);
        const char *red = zb_color(opt->color, ZB_C_RED);
        const char *reset = zb_color(opt->color, ZB_C_RESET);
        int ok = status_text != NULL && strcmp(status_text, "ok") == 0;

        zb_format_duration(uptime > 0 ? (uint64_t)uptime : 0, uptime_text,
                           sizeof(uptime_text));
        printf("%s%s%s  up %s\n", ok ? green : red,
               status_text != NULL ? status_text : "unknown", reset,
               uptime_text);
    }

    /* Exit 0 only on a genuine "ok" — anything else is a failed healthcheck. */
    if (status_text == NULL || strcmp(status_text, "ok") != 0) {
        if (rc == ZB_EXIT_OK) {
            rc = ZB_EXIT_ERROR;
        }
        if (!opt->json) {
            fputs("zhuzhbox: the service did not report status \"ok\"\n",
                  stderr);
        }
    }

cleanup:
    zb_free(positionals);
    zb_json_free(response);
    zb_client_free(client);
    zb_error_clear(&err);
    return rc;
}
