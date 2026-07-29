#include "commands/commands.h"

#include <string.h>

#include "util/buf.h"

const zb_command zb_commands[] = {
    {"upload", "Upload one or more files and print their links",
     "upload <file...> [options]", zb_cmd_upload, zb_help_upload},
    {"get", "Download a file, or list a collection", "get <link|token> [options]",
     zb_cmd_get, zb_help_get},
    {"ls", "List the uploads this machine knows about", "ls [options]",
     zb_cmd_ls, zb_help_ls},
    {"rm", "Delete an upload from the server", "rm <link|token> [options]",
     zb_cmd_rm, zb_help_rm},
    {"status", "Show what a link points at, without downloading it",
     "status <link|token>", zb_cmd_status, zb_help_status},
    {"quota", "Show your rolling weekly upload quota", "quota", zb_cmd_quota,
     zb_help_quota},
    {"stats", "Show service-wide statistics", "stats", zb_cmd_stats,
     zb_help_stats},
    {"health", "Check that the service is up", "health", zb_cmd_health,
     zb_help_health},
    {"report", "Report an upload for abuse", "report <link|token> [--note ...]",
     zb_cmd_report, zb_help_report},
    {"rules", "Print the content rules", "rules", zb_cmd_rules, zb_help_rules},
    {"config", "Get, set, or list persisted settings",
     "config <get|set|list> [key] [value]", zb_cmd_config, zb_help_config},
    {"shelf", "Export or import your local uploads list",
     "shelf <export|import|show> <path|token>", zb_cmd_shelf, zb_help_shelf},
    {NULL, NULL, NULL, NULL, NULL},
};

const zb_command *zb_command_find(const char *name)
{
    size_t i;
    if (name == NULL) {
        return NULL;
    }
    for (i = 0; zb_commands[i].name != NULL; i++) {
        if (strcmp(zb_commands[i].name, name) == 0) {
            return &zb_commands[i];
        }
    }
    return NULL;
}

/* Levenshtein distance, capped at a small matrix because every command name is
 * short. Only used to say "did you mean". */
static size_t edit_distance(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    size_t prev[32];
    size_t curr[32];
    size_t i;
    size_t k;

    if (lb >= sizeof(prev) / sizeof(prev[0])) {
        return (size_t)-1;
    }
    for (k = 0; k <= lb; k++) {
        prev[k] = k;
    }
    for (i = 1; i <= la; i++) {
        curr[0] = i;
        for (k = 1; k <= lb; k++) {
            size_t cost = (a[i - 1] == b[k - 1]) ? 0 : 1;
            size_t del = prev[k] + 1;
            size_t ins = curr[k - 1] + 1;
            size_t sub = prev[k - 1] + cost;
            size_t best = del < ins ? del : ins;
            curr[k] = best < sub ? best : sub;
        }
        for (k = 0; k <= lb; k++) {
            prev[k] = curr[k];
        }
    }
    return prev[lb];
}

const char *zb_command_suggest(const char *name)
{
    const char *best = NULL;
    size_t best_distance = (size_t)-1;
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (i = 0; zb_commands[i].name != NULL; i++) {
        size_t d = edit_distance(name, zb_commands[i].name);
        if (d < best_distance) {
            best_distance = d;
            best = zb_commands[i].name;
        }
    }
    /* Two edits is about where a suggestion stops being helpful. */
    return best_distance <= 2 ? best : NULL;
}
