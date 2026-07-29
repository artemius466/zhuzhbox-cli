#include <stdio.h>
#include <string.h>

#include "commands/commands.h"
#include "format/color.h"
#include "format/table.h"
#include "store/paths.h"
#include "util/buf.h"

void zb_help_config(FILE *out)
{
    fputs(
        "Reads and writes the persisted settings in config.json.\n"
        "\n"
        "  zhuzhbox config list          show every setting and where its\n"
        "                                current value came from\n"
        "  zhuzhbox config get KEY       print one value\n"
        "  zhuzhbox config set KEY VAL   persist one value\n"
        "\n"
        "Settings resolve in this order, most specific first: command-line\n"
        "flags, then ZHUZHBOX_* environment variables, then config.json, then\n"
        "the built-in defaults. `config set` only ever writes the file, so an\n"
        "environment override stays an override and is never silently made\n"
        "permanent.\n",
        out);
}

static int cmd_list(zb_options *opt)
{
    zb_table *table;
    size_t i;

    if (opt->json) {
        zb_json *root = zb_json_new_object();
        if (root == NULL) {
            return ZB_EXIT_ERROR;
        }
        for (i = 0; i < zb_config_key_count(); i++) {
            zb_json *item = zb_json_new_object();
            char *value = zb_config_key_value(&opt->cfg, i);
            int failed;

            if (item == NULL || value == NULL) {
                zb_json_free(item);
                zb_free(value);
                zb_json_free(root);
                return ZB_EXIT_ERROR;
            }
            failed = zb_json_obj_set_str(item, "value", value) != 0 ||
                     zb_json_obj_set_str(
                         item, "source",
                         zb_config_source_name(
                             zb_config_key_source(&opt->cfg, i))) != 0 ||
                     zb_json_obj_set_str(item, "env",
                                         zb_config_key_env(i)) != 0 ||
                     zb_json_obj_add(root, zb_config_key_name(i), item) != 0;
            zb_free(value);
            if (failed) {
                zb_json_free(root);
                return ZB_EXIT_ERROR;
            }
        }
        {
            int rc = zb_print_json(root);
            zb_json_free(root);
            return rc;
        }
    }

    table = zb_table_new(3);
    if (table == NULL) {
        return ZB_EXIT_ERROR;
    }
    (void)zb_table_header(table, 0, "KEY");
    (void)zb_table_header(table, 1, "VALUE");
    (void)zb_table_header(table, 2, "SOURCE");

    for (i = 0; i < zb_config_key_count(); i++) {
        char *value = zb_config_key_value(&opt->cfg, i);
        if (value == NULL || zb_table_row(table) != 0) {
            zb_free(value);
            zb_table_free(table);
            return ZB_EXIT_ERROR;
        }
        (void)zb_table_set(table, 0, zb_config_key_name(i));
        (void)zb_table_set(table, 1, value);
        (void)zb_table_set(
            table, 2,
            zb_config_source_name(zb_config_key_source(&opt->cfg, i)));
        zb_free(value);
    }
    zb_table_print(table, stdout, opt->color);
    zb_table_free(table);

    {
        char *path = zb_paths_file(opt->base_dir, ZB_CONFIG_FILE_NAME);
        printf("\n%s%s%s\n", zb_color(opt->color, ZB_C_DIM),
               path != NULL ? path : "(unknown config path)",
               zb_color(opt->color, ZB_C_RESET));
        zb_free(path);
    }
    return ZB_EXIT_OK;
}

int zb_cmd_config(zb_options *opt, int argc, char **argv)
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
    if (positional_count == 0) {
        zb_error_setf(&err, ZB_ERR_USAGE, "config needs get, set or list");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    action = positionals[0];

    if (strcmp(action, "list") == 0) {
        if (positional_count != 1) {
            zb_error_setf(&err, ZB_ERR_USAGE, "config list takes no arguments");
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        rc = cmd_list(opt);
        goto cleanup;
    }

    if (strcmp(action, "get") == 0) {
        int index;
        char *value;

        if (positional_count != 2) {
            zb_error_setf(&err, ZB_ERR_USAGE, "config get needs a key");
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        index = zb_config_key_index(positionals[1]);
        if (index < 0) {
            zb_error_setf(&err, ZB_ERR_USAGE,
                          "unknown setting \"%s\" — `zhuzhbox config list` "
                          "shows them all",
                          positionals[1]);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        value = zb_config_key_value(&opt->cfg, (size_t)index);
        if (value == NULL) {
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        if (opt->json) {
            zb_json *root = zb_json_new_object();
            if (root != NULL) {
                (void)zb_json_obj_set_str(root, "key",
                                          zb_config_key_name((size_t)index));
                (void)zb_json_obj_set_str(root, "value", value);
                (void)zb_json_obj_set_str(
                    root, "source",
                    zb_config_source_name(
                        zb_config_key_source(&opt->cfg, (size_t)index)));
                rc = zb_print_json(root);
                zb_json_free(root);
            }
        } else {
            printf("%s\n", value);
        }
        zb_free(value);
        goto cleanup;
    }

    if (strcmp(action, "set") == 0) {
        int index;

        if (positional_count != 3) {
            zb_error_setf(&err, ZB_ERR_USAGE, "config set needs a key and a value");
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        index = zb_config_key_index(positionals[1]);
        if (index < 0) {
            zb_error_setf(&err, ZB_ERR_USAGE,
                          "unknown setting \"%s\" — `zhuzhbox config list` "
                          "shows them all",
                          positionals[1]);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        if (zb_config_key_set(&opt->cfg, (size_t)index, positionals[2], &err) !=
            ZB_OK) {
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        if (zb_config_save(&opt->cfg, opt->base_dir, &err) != ZB_OK) {
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }
        if (zb_config_key_source(&opt->cfg, (size_t)index) == ZB_SRC_ENV) {
            fprintf(stderr,
                    "zhuzhbox: note — %s is also set in the environment (%s), "
                    "which wins over the config file.\n",
                    zb_config_key_name((size_t)index),
                    zb_config_key_env((size_t)index));
        }
        if (!opt->json) {
            char *value = zb_config_key_value(&opt->cfg, (size_t)index);
            printf("%s = %s\n", zb_config_key_name((size_t)index),
                   value != NULL ? value : positionals[2]);
            zb_free(value);
        } else {
            zb_json *root = zb_json_new_object();
            if (root != NULL) {
                (void)zb_json_obj_set_bool(root, "ok", 1);
                (void)zb_json_obj_set_str(root, "key",
                                          zb_config_key_name((size_t)index));
                rc = zb_print_json(root);
                zb_json_free(root);
            }
        }
        goto cleanup;
    }

    zb_error_setf(&err, ZB_ERR_USAGE,
                  "config takes get, set or list, not \"%s\"", action);
    rc = zb_report_error(opt, &err);

cleanup:
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
