#include <stdio.h>

#include "commands/commands.h"
#include "format/color.h"
#include "rules_generated.h"
#include "util/buf.h"

void zb_help_rules(FILE *out)
{
    fputs("Prints the content rules, copied verbatim from the site's FAQ.\n"
          "\n"
          "The text is baked into the binary at build time rather than "
          "fetched:\n"
          "there is no API endpoint that serves it, so this is a bundled copy "
          "that\n"
          "is regenerated from the website sources and checked in CI.\n",
          out);
}

int zb_cmd_rules(zb_options *opt, int argc, char **argv)
{
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    static const zb_opt_spec k_specs[] = {{NULL, 0, ZB_ARG_NONE, NULL, NULL}};
    size_t i;
    int rc = ZB_EXIT_OK;

    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, NULL, NULL, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count > 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "rules takes no arguments");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (opt->json) {
        zb_json *root = zb_json_new_object();
        zb_json *array = zb_json_new_array();

        if (root == NULL || array == NULL) {
            zb_json_free(root);
            zb_json_free(array);
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        for (i = 0; i < ZB_RULES_COUNT; i++) {
            zb_json *item = zb_json_new_object();
            if (item == NULL ||
                zb_json_obj_set_str(item, "title", ZB_RULES[i].title) != 0 ||
                zb_json_obj_set_str(item, "detail", ZB_RULES[i].detail) != 0 ||
                zb_json_arr_add(array, item) != 0) {
                zb_json_free(item);
                zb_json_free(array);
                zb_json_free(root);
                zb_error_nomem(&err);
                rc = zb_report_error(opt, &err);
                goto cleanup;
            }
        }
        if (zb_json_obj_set_str(root, "intro", ZB_RULES_INTRO) != 0 ||
            zb_json_obj_add(root, "rules", array) != 0) {
            zb_json_free(root);
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        rc = zb_print_json(root);
        zb_json_free(root);
        goto cleanup;
    }

    {
        const char *bold = zb_color(opt->color, ZB_C_BOLD);
        const char *dim = zb_color(opt->color, ZB_C_DIM);
        const char *reset = zb_color(opt->color, ZB_C_RESET);

        printf("%s%s%s\n\n", dim, ZB_RULES_INTRO, reset);
        for (i = 0; i < ZB_RULES_COUNT; i++) {
            printf("  %zu. %s%s%s\n", i + 1, bold, ZB_RULES[i].title, reset);
            printf("     %s%s%s\n", dim, ZB_RULES[i].detail, reset);
            if (i + 1 < ZB_RULES_COUNT) {
                printf("\n");
            }
        }
    }

cleanup:
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
