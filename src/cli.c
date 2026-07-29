#include "cli.h"

#include <stdarg.h>
#include <string.h>

#include "format/color.h"
#include "store/paths.h"
#include "util/buf.h"
#include "util/json.h"
#include "util/platform.h"
#include "util/str.h"

void zb_options_init(zb_options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->color = 0;
    opt->progress = 0;
}

void zb_options_free(zb_options *opt)
{
    if (opt == NULL) {
        return;
    }
    zb_free(opt->config_dir);
    zb_free(opt->api_override);
    zb_free(opt->download_override);
    zb_free(opt->site_override);
    zb_free(opt->base_dir);
    zb_config_free(&opt->cfg);
    memset(opt, 0, sizeof(*opt));
}

/* Strip one trailing slash so "https://host/" + "/v1/quota" stays well formed. */
static char *dup_host(const char *value)
{
    char *copy = zb_strdup(value);
    size_t len;
    if (copy == NULL) {
        return NULL;
    }
    len = strlen(copy);
    while (len > 0 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    return copy;
}

zb_status zb_options_take_globals(zb_options *opt, int *argc, char **argv,
                                  zb_error *err)
{
    int read_index = 0;
    int write_index = 0;
    int passthrough = 0;

    while (read_index < *argc) {
        char *arg = argv[read_index];
        const char *value = NULL;
        char **target = NULL;
        int consumes = 0;

        if (passthrough || arg[0] != '-' || arg[1] == '\0') {
            argv[write_index++] = argv[read_index++];
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            passthrough = 1;
            argv[write_index++] = argv[read_index++];
            continue;
        }

        /* Accept --name=value as well as --name value. */
        {
            char *eq = strchr(arg, '=');
            char name_buf[64];
            const char *name = arg;
            if (eq != NULL) {
                size_t n = (size_t)(eq - arg);
                if (n >= sizeof(name_buf)) {
                    argv[write_index++] = argv[read_index++];
                    continue;
                }
                memcpy(name_buf, arg, n);
                name_buf[n] = '\0';
                name = name_buf;
                value = eq + 1;
            }

            if (strcmp(name, "--api") == 0 || strcmp(name, "--api-host") == 0) {
                target = &opt->api_override;
                consumes = 1;
            } else if (strcmp(name, "--download-host") == 0) {
                target = &opt->download_override;
                consumes = 1;
            } else if (strcmp(name, "--site") == 0 ||
                       strcmp(name, "--site-host") == 0) {
                target = &opt->site_override;
                consumes = 1;
            } else if (strcmp(name, "--config") == 0) {
                target = &opt->config_dir;
                consumes = 1;
            } else if (strcmp(name, "--json") == 0) {
                opt->json = 1;
            } else if (strcmp(name, "--quiet") == 0 || strcmp(name, "-q") == 0) {
                opt->quiet = 1;
            } else if (strcmp(name, "--no-color") == 0 ||
                       strcmp(name, "--no-colour") == 0) {
                opt->no_color = 1;
            } else if (strcmp(name, "--debug") == 0) {
                opt->debug = 1;
            } else if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
                opt->want_help = 1;
            } else if (strcmp(name, "--version") == 0 ||
                       strcmp(name, "-V") == 0) {
                opt->want_version = 1;
            } else {
                /* Not ours — leave it for the subcommand parser. */
                argv[write_index++] = argv[read_index++];
                continue;
            }
        }

        read_index++;
        if (consumes) {
            char *copy;
            if (value == NULL) {
                if (read_index >= *argc) {
                    return zb_error_setf(err, ZB_ERR_USAGE,
                                         "%s needs a value", arg);
                }
                value = argv[read_index++];
            }
            copy = (target == &opt->config_dir) ? zb_strdup(value)
                                                : dup_host(value);
            if (copy == NULL) {
                return zb_error_nomem(err);
            }
            zb_free(*target);
            *target = copy;
        } else if (value != NULL) {
            return zb_error_setf(err, ZB_ERR_USAGE, "%s does not take a value",
                                 arg);
        }
    }

    *argc = write_index;
    return ZB_OK;
}

zb_status zb_options_resolve(zb_options *opt, zb_error *err)
{
    zb_status rc;

    opt->base_dir = zb_paths_base_dir(opt->config_dir);
    if (opt->base_dir == NULL) {
        return zb_error_setf(err, ZB_ERR_IO,
                             "cannot determine your config directory — set "
                             "ZHUZHBOX_CONFIG_DIR or pass --config");
    }

    rc = zb_config_defaults(&opt->cfg, err);
    if (rc != ZB_OK) {
        return rc;
    }
    rc = zb_config_load(&opt->cfg, opt->base_dir, err);
    if (rc != ZB_OK) {
        return rc;
    }

    opt->color =
        zb_color_should_enable(zb_isatty_stdout(), opt->no_color, opt->cfg.color);

    /* Progress is a TTY affordance: never in --json mode (it would pollute the
     * stream even on stderr for no benefit), never under --quiet, never when
     * stdout is redirected. */
    if (opt->json || opt->quiet) {
        opt->progress = 0;
    } else if (opt->cfg.progress == 0) {
        opt->progress = 0;
    } else if (opt->cfg.progress == 1) {
        opt->progress = 1;
    } else {
        opt->progress = zb_isatty_stderr() && zb_isatty_stdout();
    }

    return ZB_OK;
}

const char *zb_opt_api(const zb_options *opt)
{
    if (opt->api_override != NULL) {
        return opt->api_override;
    }
    return opt->cfg.api_host;
}

const char *zb_opt_download_host(const zb_options *opt)
{
    if (opt->download_override != NULL) {
        return opt->download_override;
    }
    return opt->cfg.download_host;
}

const char *zb_opt_site(const zb_options *opt)
{
    if (opt->site_override != NULL) {
        return opt->site_override;
    }
    return opt->cfg.site_host;
}

int zb_report_error(const zb_options *opt, const zb_error *err)
{
    int color = zb_color_should_enable(zb_isatty_stderr(),
                                       opt != NULL ? opt->no_color : 1, -1);
    const char *red = zb_color(color, ZB_C_RED);
    const char *reset = zb_color(color, ZB_C_RESET);

    if (err->status == ZB_ERR_NOMEM) {
        fputs("zhuzhbox: out of memory\n", stderr);
        return ZB_EXIT_ERROR;
    }

    /* The server's own `error` string is the message; we only add the
     * program name so it is obvious who is talking. */
    fprintf(stderr, "%szhuzhbox:%s %s\n", red, reset, zb_error_message(err));

    if (opt != NULL && opt->json) {
        zb_json *obj = zb_json_new_object();
        if (obj != NULL) {
            char *text;
            (void)zb_json_obj_set_bool(obj, "ok", 0);
            (void)zb_json_obj_set_str(obj, "error", zb_error_message(err));
            (void)zb_json_obj_set_str(obj, "kind", zb_status_name(err->status));
            if (err->http_status > 0) {
                (void)zb_json_obj_set_i64(obj, "status", err->http_status);
            }
            text = zb_json_print(obj, 0);
            if (text != NULL) {
                fprintf(stderr, "%s\n", text);
                zb_free(text);
            }
            zb_json_free(obj);
        }
    }

    switch (err->status) {
    case ZB_ERR_USAGE:
        return ZB_EXIT_USAGE;
    case ZB_ERR_CANCELED:
        return ZB_EXIT_INTERRUPTED;
    default:
        return ZB_EXIT_ERROR;
    }
}

void zb_info(const zb_options *opt, const char *fmt, ...)
{
    va_list ap;

    if (opt != NULL && (opt->quiet || opt->json)) {
        return;
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int zb_print_json(const zb_json *value)
{
    char *text = zb_json_print(value, 1);
    if (text == NULL) {
        fputs("zhuzhbox: out of memory\n", stderr);
        return ZB_EXIT_ERROR;
    }
    printf("%s\n", text);
    zb_free(text);
    return ZB_EXIT_OK;
}

int zb_confirm(const zb_options *opt, int assume_yes, zb_error *err,
               const char *fmt, ...)
{
    va_list ap;
    char answer[16];

    if (assume_yes) {
        return 1;
    }

    /* Never prompt when there is nobody to answer: hanging a pipeline forever
     * is worse than failing it with a message that says what to pass. */
    if (!zb_isatty_stdin() || !zb_isatty_stderr() || (opt != NULL && opt->json)) {
        zb_error_setf(err, ZB_ERR_USAGE,
                      "this needs confirmation but there is no terminal to ask "
                      "on — pass --yes to proceed");
        return -1;
    }

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs(" [y/N] ", stderr);
    fflush(stderr);

    if (fgets(answer, sizeof(answer), stdin) == NULL) {
        return 0;
    }
    return answer[0] == 'y' || answer[0] == 'Y';
}

/* ------------------------------------------------------------------ */
/* Option parsing                                                       */
/* ------------------------------------------------------------------ */

static const zb_opt_spec *find_long(const zb_opt_spec *specs, const char *name,
                                    size_t len)
{
    size_t i;
    for (i = 0; specs[i].long_name != NULL; i++) {
        if (strlen(specs[i].long_name) == len &&
            strncmp(specs[i].long_name, name, len) == 0) {
            return &specs[i];
        }
    }
    return NULL;
}

static const zb_opt_spec *find_short(const zb_opt_spec *specs, char c)
{
    size_t i;
    for (i = 0; specs[i].long_name != NULL; i++) {
        if (specs[i].short_name == c) {
            return &specs[i];
        }
    }
    return NULL;
}

zb_status zb_parse_args(int argc, char **argv, const zb_opt_spec *specs,
                        zb_opt_handler handler, void *ctx,
                        char ***positionals, size_t *positional_count,
                        zb_error *err)
{
    char **pos;
    size_t npos = 0;
    int i = 0;
    int only_positional = 0;
    zb_status rc = ZB_OK;

    *positionals = NULL;
    *positional_count = 0;

    if (argc < 0) {
        return zb_error_setf(err, ZB_ERR_USAGE, "invalid arguments");
    }
    pos = zb_calloc((size_t)argc + 1, sizeof(*pos));
    if (pos == NULL) {
        return zb_error_nomem(err);
    }

    for (i = 0; i < argc; i++) {
        char *arg = argv[i];

        if (only_positional || arg[0] != '-' || arg[1] == '\0') {
            pos[npos++] = arg;
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            only_positional = 1;
            continue;
        }

        if (arg[1] == '-') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            size_t name_len = eq != NULL ? (size_t)(eq - name) : strlen(name);
            const zb_opt_spec *spec = find_long(specs, name, name_len);
            const char *value = NULL;

            if (spec == NULL) {
                rc = zb_error_setf(err, ZB_ERR_USAGE, "unknown option %s", arg);
                goto done;
            }
            if (spec->kind == ZB_ARG_REQUIRED) {
                if (eq != NULL) {
                    value = eq + 1;
                } else if (i + 1 < argc) {
                    value = argv[++i];
                } else {
                    rc = zb_error_setf(err, ZB_ERR_USAGE,
                                       "--%s needs a value", spec->long_name);
                    goto done;
                }
            } else if (eq != NULL) {
                rc = zb_error_setf(err, ZB_ERR_USAGE,
                                   "--%s does not take a value",
                                   spec->long_name);
                goto done;
            }
            rc = handler(ctx, spec, value, err);
            if (rc != ZB_OK) {
                goto done;
            }
            continue;
        }

        /* Short options, possibly clustered: -abc, or -o value, or -ovalue. */
        {
            const char *p = arg + 1;
            while (*p != '\0') {
                const zb_opt_spec *spec = find_short(specs, *p);
                const char *value = NULL;

                if (spec == NULL) {
                    rc = zb_error_setf(err, ZB_ERR_USAGE, "unknown option -%c",
                                       *p);
                    goto done;
                }
                if (spec->kind == ZB_ARG_REQUIRED) {
                    if (p[1] != '\0') {
                        value = p + 1;
                    } else if (i + 1 < argc) {
                        value = argv[++i];
                    } else {
                        rc = zb_error_setf(err, ZB_ERR_USAGE,
                                           "-%c needs a value", *p);
                        goto done;
                    }
                    rc = handler(ctx, spec, value, err);
                    if (rc != ZB_OK) {
                        goto done;
                    }
                    break;
                }
                rc = handler(ctx, spec, NULL, err);
                if (rc != ZB_OK) {
                    goto done;
                }
                p++;
            }
        }
    }

done:
    if (rc != ZB_OK) {
        zb_free(pos);
        return rc;
    }
    *positionals = pos;
    *positional_count = npos;
    return ZB_OK;
}

void zb_print_options(FILE *out, const zb_opt_spec *specs)
{
    size_t i;
    size_t width = 0;

    for (i = 0; specs[i].long_name != NULL; i++) {
        /* Always 4 for the "-x, " column, whether or not there is a short
         * form, plus 2 for "--". */
        size_t w = strlen(specs[i].long_name) + 6;
        if (specs[i].kind == ZB_ARG_REQUIRED && specs[i].metavar != NULL) {
            w += strlen(specs[i].metavar) + 1;
        }
        if (w > width) {
            width = w;
        }
    }

    for (i = 0; specs[i].long_name != NULL; i++) {
        zb_buf line;
        zb_buf_init(&line);
        if (specs[i].short_name != 0) {
            (void)zb_buf_printf(&line, "-%c, ", specs[i].short_name);
        } else {
            (void)zb_buf_append_str(&line, "    ");
        }
        (void)zb_buf_printf(&line, "--%s", specs[i].long_name);
        if (specs[i].kind == ZB_ARG_REQUIRED && specs[i].metavar != NULL) {
            (void)zb_buf_printf(&line, " %s", specs[i].metavar);
        }
        fprintf(out, "  %-*s  %s\n", (int)width, zb_buf_str(&line),
                specs[i].help != NULL ? specs[i].help : "");
        zb_buf_free(&line);
    }
}
