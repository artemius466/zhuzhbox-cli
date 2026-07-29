/* main.c — entry point, global option handling, subcommand dispatch.
 *
 * main is the only function that decides the process exit code, and the only
 * place a command's result becomes one. Nothing deeper calls exit(). */

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "commands/commands.h"
#include "format/color.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/platform.h"

static void print_version(void)
{
    curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    printf("zhuzhbox %s\n", ZB_VERSION);
    if (info != NULL) {
        printf("libcurl %s", info->version);
        if (info->ssl_version != NULL) {
            printf(" (%s)", info->ssl_version);
        }
        printf("\n");
    }
}

static void print_usage(FILE *out, int color)
{
    const char *bold = zb_color(color, ZB_C_BOLD);
    const char *dim = zb_color(color, ZB_C_DIM);
    const char *reset = zb_color(color, ZB_C_RESET);
    size_t i;
    size_t width = 0;

    fprintf(out, "%szhuzhbox%s — anonymous file sharing from the command line\n\n",
            bold, reset);
    fprintf(out, "%sUsage:%s zhuzhbox <command> [options]\n\n", bold, reset);

    for (i = 0; zb_commands[i].name != NULL; i++) {
        size_t n = strlen(zb_commands[i].name);
        if (n > width) {
            width = n;
        }
    }

    fprintf(out, "%sCommands:%s\n", bold, reset);
    for (i = 0; zb_commands[i].name != NULL; i++) {
        fprintf(out, "  %-*s  %s\n", (int)width, zb_commands[i].name,
                zb_commands[i].summary);
    }

    fprintf(out, "\n%sGlobal options:%s\n", bold, reset);
    fputs("      --api URL            override the API base URL\n", out);
    fputs("      --download-host URL  override the download host\n", out);
    fputs("      --site URL           override the site host used in links\n",
          out);
    fputs("      --config DIR         use an alternate config/shelf directory\n",
          out);
    fputs("      --json               emit JSON on stdout instead of text\n",
          out);
    fputs("  -q, --quiet              suppress progress and status lines\n", out);
    fputs("      --no-color           disable ANSI color (also NO_COLOR)\n", out);
    fputs("      --debug              verbose diagnostics on stderr\n", out);
    fputs("  -h, --help               show this help\n", out);
    fputs("  -V, --version            show the version\n", out);

    fprintf(out,
            "\n%sRun `zhuzhbox <command> --help` for the details of one "
            "command.%s\n",
            dim, reset);
    fprintf(out,
            "%szhuzhbox has no accounts: the only record of what you uploaded "
            "is the local\nshelf. `zhuzhbox shelf export` is the backup.%s\n",
            dim, reset);
}

static void print_command_help(const zb_command *cmd, int color)
{
    const char *bold = zb_color(color, ZB_C_BOLD);
    const char *reset = zb_color(color, ZB_C_RESET);

    printf("%sUsage:%s zhuzhbox %s\n\n", bold, reset, cmd->usage);
    printf("%s\n", cmd->summary);
    if (cmd->help != NULL) {
        printf("\n");
        cmd->help(stdout);
    }
}

int main(int argc, char **argv)
{
    zb_options opt;
    zb_error err;
    int rest_argc;
    const zb_command *cmd;
    int rc = ZB_EXIT_OK;
    int u_argc = argc;
    char **u_argv = argv; /* falls back to the CRT's argv if the below fails */
    int owned_argc = 0;
    char **owned_argv = NULL; /* the actual owned strings, for freeing only */

    zb_console_init();
    zb_install_signal_handlers();

    /* On Windows the CRT's own argv is built through the active ANSI code
     * page, which mangles any argument that page cannot represent — a
     * Cyrillic filename on the default Western code page, for instance —
     * before this function ever runs. Reconstruct argv from the real (wide)
     * command line so it is reliably UTF-8 regardless. A no-op duplication on
     * POSIX, where argv is already UTF-8. */
    if (zb_argv_utf8(argc, argv, &owned_argc, &owned_argv) == 0) {
        /* u_argv is a shallow COPY of the pointer array, not owned_argv
         * itself: zb_options_take_globals compacts its argv in place, and
         * that compaction can leave the same string pointer duplicated
         * across two slots (verified — a flag consumed with no value ahead
         * of a surviving positional aliases exactly this way). Freeing
         * owned_argv element-by-element after compaction had touched it
         * would double-free whichever string ended up duplicated. Handing
         * the parser a disposable copy of the pointers, and freeing only
         * from the untouched owned_argv, keeps every string freed exactly
         * once regardless of how the parser reorders its own copy. */
        char **working = zb_calloc((size_t)owned_argc, sizeof(*working));
        if (working != NULL) {
            memcpy(working, owned_argv, (size_t)owned_argc * sizeof(*working));
            u_argc = owned_argc;
            u_argv = working;
        } else {
            zb_argv_free(owned_argc, owned_argv);
            owned_argc = 0;
            owned_argv = NULL;
        }
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fputs("zhuzhbox: could not initialize libcurl\n", stderr);
        if (u_argv != argv && u_argv != owned_argv) {
            zb_free(u_argv);
        }
        zb_argv_free(owned_argc, owned_argv);
        return ZB_EXIT_ERROR;
    }

    zb_options_init(&opt);
    zb_error_init(&err);

    /* argv[0] is the program name; everything after it is ours. */
    rest_argc = u_argc - 1;
    if (rest_argc < 0) {
        rest_argc = 0;
    }
    if (zb_options_take_globals(&opt, &rest_argc, u_argv + 1, &err) != ZB_OK) {
        rc = zb_report_error(&opt, &err);
        goto cleanup;
    }

    if (opt.want_version) {
        print_version();
        goto cleanup;
    }

    if (rest_argc == 0) {
        int color = zb_color_should_enable(zb_isatty_stdout(), opt.no_color, -1);
        print_usage(opt.want_help ? stdout : stderr, color);
        rc = opt.want_help ? ZB_EXIT_OK : ZB_EXIT_USAGE;
        goto cleanup;
    }

    /* take_globals compacted u_argv+1 in place, so the subcommand is
     * u_argv[1]. */
    cmd = zb_command_find(u_argv[1]);
    if (cmd == NULL) {
        const char *suggestion = zb_command_suggest(u_argv[1]);
        int color = zb_color_should_enable(zb_isatty_stderr(), opt.no_color, -1);
        fprintf(stderr, "%szhuzhbox:%s unknown command \"%s\"\n",
                zb_color(color, ZB_C_RED), zb_color(color, ZB_C_RESET),
                u_argv[1]);
        if (suggestion != NULL) {
            fprintf(stderr, "Did you mean \"%s\"?\n", suggestion);
        }
        fprintf(stderr, "Run `zhuzhbox --help` for the list of commands.\n");
        rc = ZB_EXIT_USAGE;
        goto cleanup;
    }

    if (opt.want_help) {
        int color = zb_color_should_enable(zb_isatty_stdout(), opt.no_color, -1);
        print_command_help(cmd, color);
        goto cleanup;
    }

    if (zb_options_resolve(&opt, &err) != ZB_OK) {
        rc = zb_report_error(&opt, &err);
        goto cleanup;
    }

    rc = cmd->run(&opt, rest_argc - 1, u_argv + 2);

cleanup:
    zb_error_clear(&err);
    zb_options_free(&opt);
    curl_global_cleanup();
    /* u_argv (the working copy of pointers) and owned_argv (the actual owned
     * strings) are freed separately and in this order deliberately — see the
     * comment where u_argv is built. */
    if (u_argv != argv && u_argv != owned_argv) {
        zb_free(u_argv);
    }
    zb_argv_free(owned_argc, owned_argv);
    /* A closed stdout (`zhuzhbox ls | head -1`) should not be reported as a
     * successful run that silently lost its output. */
    if (rc == ZB_EXIT_OK && fflush(stdout) != 0) {
        rc = ZB_EXIT_ERROR;
    }
    return rc;
}
