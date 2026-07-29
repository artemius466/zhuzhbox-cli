/* commands.h — one dispatch table entry per subcommand.
 *
 * Every run() returns a process exit code and has already printed whatever the
 * user needs to see. Nothing below main() terminates the process directly, so
 * the session and shelf flush on the way out always happens. */
#ifndef ZB_COMMANDS_H
#define ZB_COMMANDS_H

#include <stdio.h>

#include "cli.h"

typedef struct {
    const char *name;
    const char *summary; /* one line, shown in `zhuzhbox --help` */
    const char *usage;   /* "upload <file...> [options]" */
    int (*run)(zb_options *opt, int argc, char **argv);
    void (*help)(FILE *out); /* long help for `zhuzhbox <cmd> --help` */
} zb_command;

extern const zb_command zb_commands[];

/* NULL when there is no such command. */
const zb_command *zb_command_find(const char *name);

/* The closest command name to `name` by edit distance, or NULL if nothing is
 * close enough to be worth suggesting. */
const char *zb_command_suggest(const char *name);

int zb_cmd_upload(zb_options *opt, int argc, char **argv);
int zb_cmd_get(zb_options *opt, int argc, char **argv);
int zb_cmd_ls(zb_options *opt, int argc, char **argv);
int zb_cmd_rm(zb_options *opt, int argc, char **argv);
int zb_cmd_status(zb_options *opt, int argc, char **argv);
int zb_cmd_quota(zb_options *opt, int argc, char **argv);
int zb_cmd_stats(zb_options *opt, int argc, char **argv);
int zb_cmd_health(zb_options *opt, int argc, char **argv);
int zb_cmd_report(zb_options *opt, int argc, char **argv);
int zb_cmd_rules(zb_options *opt, int argc, char **argv);
int zb_cmd_config(zb_options *opt, int argc, char **argv);
int zb_cmd_shelf(zb_options *opt, int argc, char **argv);

void zb_help_upload(FILE *out);
void zb_help_get(FILE *out);
void zb_help_ls(FILE *out);
void zb_help_rm(FILE *out);
void zb_help_status(FILE *out);
void zb_help_quota(FILE *out);
void zb_help_stats(FILE *out);
void zb_help_health(FILE *out);
void zb_help_report(FILE *out);
void zb_help_rules(FILE *out);
void zb_help_config(FILE *out);
void zb_help_shelf(FILE *out);

#endif /* ZB_COMMANDS_H */
