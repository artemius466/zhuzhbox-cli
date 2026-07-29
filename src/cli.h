/* cli.h — hand-rolled argument parsing and the global option set.
 *
 * getopt_long is not available on MSVC and GNU permutation behavior differs
 * across libcs, so the parser here is ours: it gives full control of the help
 * text and the error messages, which is most of what a CLI is judged on. */
#ifndef ZB_CLI_H
#define ZB_CLI_H

#include <stddef.h>
#include <stdio.h>

#include "store/config.h"
#include "util/error.h"
#include "util/json.h"

#define ZB_VERSION "1.0.0"

/* Exit codes (§5). */
#define ZB_EXIT_OK 0
#define ZB_EXIT_ERROR 1
#define ZB_EXIT_USAGE 2
#define ZB_EXIT_INTERRUPTED 130

typedef struct {
    /* Raw global flags. */
    int json;
    int quiet;
    int no_color;
    int debug;
    int want_help;
    int want_version;
    char *config_dir; /* owned; from --config */

    /* Host overrides — this invocation only, never persisted. */
    char *api_override;      /* owned */
    char *download_override; /* owned */
    char *site_override;     /* owned */

    /* Resolved after config load. */
    zb_config cfg;
    char *base_dir; /* owned; the config/shelf directory */

    /* Effective output decisions. */
    int color;    /* color escapes on stdout */
    int progress; /* draw progress bars */
} zb_options;

void zb_options_init(zb_options *opt);
void zb_options_free(zb_options *opt);

/* Pull the global flags out of argv, compacting the array in place so what is
 * left is the subcommand and its own arguments. Global flags are accepted both
 * before and after the subcommand name. Stops at "--". */
zb_status zb_options_take_globals(zb_options *opt, int *argc, char **argv,
                                  zb_error *err);

/* Load config.json and the environment, then resolve color/progress. */
zb_status zb_options_resolve(zb_options *opt, zb_error *err);

/* The API/download/site base URLs to actually use, after overrides. */
const char *zb_opt_api(const zb_options *opt);
const char *zb_opt_download_host(const zb_options *opt);
const char *zb_opt_site(const zb_options *opt);

/* Print `err` to stderr in the house style and return the exit code that goes
 * with it. In --json mode the error is additionally emitted as a JSON object
 * on stderr so a wrapper script can parse it without scraping prose. */
int zb_report_error(const zb_options *opt, const zb_error *err);

/* Status line to stderr, suppressed by --quiet. */
void zb_info(const zb_options *opt, const char *fmt, ...);

/* Print a JSON value to stdout followed by a newline — the only thing that
 * goes to stdout in --json mode. Returns an exit code. */
int zb_print_json(const zb_json *value);

/* Ask a yes/no question on stderr. Returns 1 for yes, 0 for no.
 *
 * With --yes, returns 1 without asking. When stdin is not a TTY it does not
 * prompt at all: it fills `err` with an actionable message and returns -1, so
 * a script fails fast instead of hanging forever on a read that never
 * completes. */
int zb_confirm(const zb_options *opt, int assume_yes, zb_error *err,
               const char *fmt, ...);

/* ---------- option specs ---------- */

typedef enum { ZB_ARG_NONE = 0, ZB_ARG_REQUIRED } zb_arg_kind;

typedef struct {
    const char *long_name; /* without the leading "--"; NULL ends the table */
    char short_name;       /* 0 when there is none */
    zb_arg_kind kind;
    const char *metavar; /* shown in help for ZB_ARG_REQUIRED */
    const char *help;
} zb_opt_spec;

/* Called once per recognized option. Return ZB_OK to continue. */
typedef zb_status (*zb_opt_handler)(void *ctx, const zb_opt_spec *spec,
                                    const char *value, zb_error *err);

/* Parse `argv[0..argc)` (which excludes the program and subcommand names).
 *
 * Supports "--name value", "--name=value", "-n value", "-nvalue", clustered
 * short flags that take no argument, and "--" to end option parsing. On
 * success `*positionals` points into argv — it borrows, and must not be
 * freed — and `*positional_count` is how many there are. */
zb_status zb_parse_args(int argc, char **argv, const zb_opt_spec *specs,
                        zb_opt_handler handler, void *ctx,
                        char ***positionals, size_t *positional_count,
                        zb_error *err);

/* Render "  --flag VALUE   help" lines for a spec table. */
void zb_print_options(FILE *out, const zb_opt_spec *specs);

#endif /* ZB_CLI_H */
