#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "api/meta.h"
#include "commands/commands.h"
#include "format/color.h"
#include "zb_limits.h"
#include "util/buf.h"
#include "util/str.h"
#include "util/token.h"

void zb_help_report(FILE *out)
{
    fputs("Reports an upload for breaking the content rules (`zhuzhbox rules`).\n"
          "\n"
          "Accepts a full link, a bare path, or just the token; the token is\n"
          "extracted locally, so an input that is not a zhuzhbox link is\n"
          "rejected here rather than sent to the server.\n"
          "\n"
          "Options:\n"
          "  -n, --note TEXT   what is wrong with it (max 2000 characters)\n"
          "\n"
          "Reporting the same link twice is not an error — the server says so\n"
          "and this prints a different message for it.\n",
          out);
}

typedef struct {
    char *note;
} report_args;

static const zb_opt_spec k_specs[] = {
    {"note", 'n', ZB_ARG_REQUIRED, "TEXT", "what is wrong with it"},
    {NULL, 0, ZB_ARG_NONE, NULL, NULL},
};

static zb_status on_option(void *ctx, const zb_opt_spec *spec,
                           const char *value, zb_error *err)
{
    report_args *args = ctx;

    if (strcmp(spec->long_name, "note") == 0) {
        char *copy = zb_strdup(value);
        if (copy == NULL) {
            return zb_error_nomem(err);
        }
        zb_free(args->note);
        args->note = copy;
    }
    return ZB_OK;
}

int zb_cmd_report(zb_options *opt, int argc, char **argv)
{
    report_args args;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    zb_client *client = NULL;
    zb_json *response = NULL;
    char *token = NULL;
    char *link = NULL;
    int duplicate = 0;
    int rc = ZB_EXIT_OK;

    memset(&args, 0, sizeof(args));
    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, on_option, &args, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count != 1) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "report takes exactly one link or token");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    /* Mirror the site's extractToken() before sending: a bad input should fail
     * here, not after a round trip. */
    token = zb_extract_token(positionals[0]);
    if (token == NULL) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "\"%s\" is not a zhuzhbox link — it should look like "
                      "%s/d/<token>",
                      positionals[0], zb_opt_site(opt));
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (args.note != NULL &&
        zb_utf8_len(args.note) > ZB_MAX_REPORT_NOTE_CHARS) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "the note is %llu characters; the limit is %d. Shorten "
                      "it rather than letting it be cut off mid-sentence.",
                      (unsigned long long)zb_utf8_len(args.note),
                      ZB_MAX_REPORT_NOTE_CHARS);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    link = zb_asprintf("%s/d/%s", zb_opt_site(opt), token);
    if (link == NULL) {
        zb_error_nomem(&err);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (zb_meta_report(client, link, args.note, &response, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    (void)zb_json_get_bool(response, "duplicate", &duplicate);

    if (opt->json) {
        rc = zb_print_json(response);
    } else if (duplicate) {
        printf("%sAlready reported%s — thanks anyway.\n",
               zb_color(opt->color, ZB_C_GREEN),
               zb_color(opt->color, ZB_C_RESET));
    } else {
        printf("%sReported%s %s\n", zb_color(opt->color, ZB_C_GREEN),
               zb_color(opt->color, ZB_C_RESET), link);
    }

cleanup:
    zb_free(link);
    zb_free(token);
    zb_free(args.note);
    zb_json_free(response);
    zb_client_free(client);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
