#include <stdio.h>
#include <string.h>

#include "api/client.h"
#include "api/meta.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/progress.h"
#include "format/table.h"
#include "format/time.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/str.h"
#include "util/token.h"

void zb_help_get(FILE *out)
{
    fputs(
        "Accepts a full link, a bare /d/<token> path, or just the token.\n"
        "\n"
        "For a file, downloads it. For a collection, lists the members without\n"
        "downloading anything — pass --all to fetch them.\n"
        "\n"
        "Options:\n"
        "  -o, --output PATH   where to write it; - means stdout\n"
        "      --all           download every member of a collection\n"
        "  -f, --force         overwrite an existing file\n"
        "\n"
        "Downloads land in <target>.part and are renamed into place only once\n"
        "they are complete, so an interrupted run never leaves something that\n"
        "looks like a finished file. Re-running resumes from the .part file\n"
        "with a Range request; if the server answers 200 instead of 206 the\n"
        "partial file is discarded and the download restarts rather than\n"
        "splicing two different prefixes together.\n"
        "\n"
        "Filenames that come back from the server are treated as untrusted:\n"
        "path separators, .. and control characters are stripped, so a\n"
        "malicious record cannot write outside the target directory.\n",
        out);
}

typedef struct {
    char *output;
    int all;
    int force;
} get_args;

static const zb_opt_spec k_specs[] = {
    {"output", 'o', ZB_ARG_REQUIRED, "PATH", "where to write it; - is stdout"},
    {"all", 0, ZB_ARG_NONE, NULL, "download every member of a collection"},
    {"force", 'f', ZB_ARG_NONE, NULL, "overwrite an existing file"},
    {NULL, 0, ZB_ARG_NONE, NULL, NULL},
};

static zb_status on_option(void *ctx, const zb_opt_spec *spec,
                           const char *value, zb_error *err)
{
    get_args *args = ctx;

    if (strcmp(spec->long_name, "output") == 0) {
        char *copy = zb_strdup(value);
        if (copy == NULL) {
            return zb_error_nomem(err);
        }
        zb_free(args->output);
        args->output = copy;
    } else if (strcmp(spec->long_name, "all") == 0) {
        args->all = 1;
    } else if (strcmp(spec->long_name, "force") == 0) {
        args->force = 1;
    }
    return ZB_OK;
}

/* ------------------------------------------------------------------ */
/* Downloading                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    zb_progress progress;
} download_ctx;

static void on_download_progress(void *ctx, uint64_t now, uint64_t total)
{
    download_ctx *dl = ctx;
    if (total > 0) {
        zb_progress_set_total(&dl->progress, total);
    }
    zb_progress_update(&dl->progress, now);
}

/* Perform one GET of the download URL into `sink`, optionally resuming from
 * `resume_from`. Sets *restart when the server ignored our Range. */
static zb_status download_once(zb_client *client, zb_options *opt,
                               const char *url, FILE *sink,
                               uint64_t resume_from, uint64_t expected_total,
                               const char *label, int *restart, zb_error *err)
{
    zb_xfer xfer;
    CURL *easy = NULL;
    download_ctx dl;
    CURLcode code;
    zb_status rc;

    *restart = 0;
    zb_xfer_init(&xfer);
    memset(&dl, 0, sizeof(dl));

    easy = zb_client_easy(client, &xfer, err);
    if (easy == NULL) {
        zb_xfer_free(&xfer);
        return err->status;
    }

    xfer.sink = sink;
    xfer.uploading = 0;
    xfer.base_offset = resume_from;
    if (resume_from > 0) {
        xfer.require_partial = 1;
        curl_easy_setopt(easy, CURLOPT_RESUME_FROM_LARGE,
                         (curl_off_t)resume_from);
    }

    zb_progress_init(&dl.progress, label, expected_total, opt->progress);
    xfer.on_progress = on_download_progress;
    xfer.progress_ctx = &dl;

    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);

    code = curl_easy_perform(easy);

    zb_progress_clear(&dl.progress);

    /* The server answered 200 to a Range request, so whatever is already in
     * the .part file may be the head of a different response. Throw it away
     * and start over rather than splicing two prefixes together.
     *
     * libcurl usually catches this itself and reports CURLE_RANGE_ERROR; the
     * write-callback check is the backstop for the cases where it does not. */
    if (xfer.restart_needed ||
        (code == CURLE_RANGE_ERROR && resume_from > 0)) {
        *restart = 1;
        rc = ZB_OK;
        goto cleanup;
    }
    if (code != CURLE_OK) {
        if (xfer.sink_failed) {
            rc = zb_error_setf(err, ZB_ERR_IO,
                               "could not write the downloaded data to disk");
        } else {
            rc = zb_client_curl_error(code, url, err);
        }
        goto cleanup;
    }

    rc = zb_client_finish(client, easy, &xfer, url, NULL, NULL, err);
    if (rc == ZB_OK) {
        zb_progress_finish(&dl.progress);
    }

cleanup:
    zb_progress_free(&dl.progress);
    if (easy != NULL) {
        curl_easy_cleanup(easy);
    }
    zb_xfer_free(&xfer);
    return rc;
}

/* Download `url` to `target`, going through <target>.part and renaming on
 * success. `target` of NULL means stdout. */
static zb_status download_to_path(zb_client *client, zb_options *opt,
                                  const char *url, const char *target,
                                  uint64_t expected_size, const char *label,
                                  int force, zb_error *err)
{
    char *part_path = NULL;
    FILE *sink = NULL;
    uint64_t resume_from = 0;
    int restart = 0;
    int attempt;
    zb_status rc;

    if (target == NULL) {
        /* Straight to stdout so `get <link> -o - | wc -c` works, including on
         * Windows where the stream would otherwise translate newlines. */
        zb_stdout_binary();
        return download_once(client, opt, url, stdout, 0, expected_size, label,
                             &restart, err);
    }

    if (!force && zb_path_exists(target)) {
        return zb_error_setf(err, ZB_ERR_IO,
                             "%s already exists — pass --force to overwrite it "
                             "or -o to choose another name",
                             target);
    }

    part_path = zb_asprintf("%s.part", target);
    if (part_path == NULL) {
        return zb_error_nomem(err);
    }

    for (attempt = 0; attempt < 2; attempt++) {
        zb_stat_info info;

        resume_from = 0;
        if (attempt == 0 && zb_stat(part_path, &info) == 0 && info.size > 0 &&
            (expected_size == 0 || info.size < expected_size)) {
            resume_from = info.size;
            sink = zb_fopen(part_path, "ab");
            if (sink != NULL) {
                zb_info(opt, "resuming from %llu bytes already downloaded",
                        (unsigned long long)resume_from);
            }
        }
        if (sink == NULL) {
            resume_from = 0;
            sink = zb_fopen(part_path, "wb");
        }
        if (sink == NULL) {
            rc = zb_error_setf(err, ZB_ERR_IO, "cannot write to %s", part_path);
            goto cleanup;
        }

        rc = download_once(client, opt, url, sink, resume_from, expected_size,
                           label, &restart, err);
        if (fclose(sink) != 0 && rc == ZB_OK) {
            rc = zb_error_setf(err, ZB_ERR_IO, "cannot finish writing %s",
                               part_path);
        }
        sink = NULL;

        if (restart) {
            zb_remove(part_path);
            continue;
        }
        break;
    }

    if (rc != ZB_OK) {
        /* Keep the .part file: a later run resumes from it. Only a completed
         * download is ever renamed into place. */
        goto cleanup;
    }

    if (zb_rename_replace(part_path, target) != 0) {
        rc = zb_error_setf(err, ZB_ERR_IO, "cannot move %s into place",
                           part_path);
        goto cleanup;
    }
    rc = ZB_OK;

cleanup:
    if (sink != NULL) {
        fclose(sink);
    }
    zb_free(part_path);
    return rc;
}

static char *download_url(const zb_options *opt, const char *token)
{
    return zb_asprintf("%s/d/%s", zb_opt_download_host(opt), token);
}

/* ------------------------------------------------------------------ */
/* Collections                                                          */
/* ------------------------------------------------------------------ */

static int list_collection(zb_options *opt, const zb_json *record)
{
    const zb_json *files = zb_json_get(record, "files");
    size_t count = zb_json_array_len(files);
    size_t i;
    zb_table *table;
    const char *title = NULL;
    const char *description = NULL;
    const char *expires = NULL;
    uint64_t total_size = 0;

    (void)zb_json_get_str(record, "title", &title);
    (void)zb_json_get_str(record, "description", &description);
    (void)zb_json_get_str(record, "expiresAt", &expires);
    (void)zb_json_get_u64(record, "totalSize", &total_size);

    {
        const char *bold = zb_color(opt->color, ZB_C_BOLD);
        const char *dim = zb_color(opt->color, ZB_C_DIM);
        const char *reset = zb_color(opt->color, ZB_C_RESET);
        char size_text[ZB_BYTES_BUF];
        char expiry[ZB_TIME_BUF];

        zb_format_bytes(total_size, size_text, sizeof(size_text));
        printf("%s%s%s\n", bold,
               title != NULL && title[0] != '\0' ? title : "(untitled bundle)",
               reset);
        if (description != NULL && description[0] != '\0') {
            printf("%s\n", description);
        }
        expiry[0] = '\0';
        if (expires != NULL) {
            int64_t epoch = 0;
            if (zb_time_parse_iso8601(expires, &epoch) == 0) {
                zb_time_relative(epoch, zb_now_unix(), expiry, sizeof(expiry));
            }
        }
        printf("%s%llu file%s · %s%s%s\n\n", dim, (unsigned long long)count,
               count == 1 ? "" : "s", size_text,
               expiry[0] != '\0' ? " · expires " : "",
               expiry[0] != '\0' ? expiry : "");
        (void)reset;
    }

    table = zb_table_new(3);
    if (table == NULL) {
        return ZB_EXIT_ERROR;
    }
    (void)zb_table_header(table, 0, "NAME");
    (void)zb_table_header(table, 1, "SIZE");
    (void)zb_table_header(table, 2, "TOKEN");
    (void)zb_table_align_right(table, 1);

    for (i = 0; i < count; i++) {
        const zb_json *file = zb_json_at(files, i);
        const char *name = NULL;
        const char *token = NULL;
        uint64_t size = 0;
        char size_text[ZB_BYTES_BUF];

        (void)zb_json_get_str(file, "filename", &name);
        (void)zb_json_get_str(file, "token", &token);
        (void)zb_json_get_u64(file, "size", &size);
        zb_format_bytes(size, size_text, sizeof(size_text));

        if (zb_table_row(table) != 0) {
            zb_table_free(table);
            return ZB_EXIT_ERROR;
        }
        (void)zb_table_set(table, 0, name != NULL ? name : "(unnamed)");
        (void)zb_table_set(table, 1, size_text);
        (void)zb_table_set(table, 2, token != NULL ? token : "");
    }
    zb_table_print(table, stdout, opt->color);
    zb_table_free(table);

    printf("\n%sPass --all to download every member.%s\n",
           zb_color(opt->color, ZB_C_DIM), zb_color(opt->color, ZB_C_RESET));
    return ZB_EXIT_OK;
}

static int download_collection(zb_client *client, zb_options *opt,
                               const zb_json *record, const char *token,
                               const get_args *args, zb_error *err)
{
    const zb_json *files = zb_json_get(record, "files");
    size_t count = zb_json_array_len(files);
    size_t i;
    char *dir = NULL;
    int rc = ZB_EXIT_OK;

    if (count == 0) {
        zb_error_setf(err, ZB_ERR_HTTP, "this bundle has no files in it");
        return zb_report_error(opt, err);
    }

    if (args->output != NULL) {
        dir = zb_strdup(args->output);
    } else {
        const char *title = NULL;
        /* A collection title is server-supplied text: sanitize it before it
         * becomes a directory name. */
        if (zb_json_get_str(record, "title", &title) && title[0] != '\0') {
            dir = zb_sanitize_dirname(title);
        } else {
            dir = zb_sanitize_dirname(token);
        }
    }
    if (dir == NULL) {
        zb_error_nomem(err);
        return zb_report_error(opt, err);
    }
    if (zb_mkdir_p(dir) != 0) {
        zb_error_setf(err, ZB_ERR_IO, "cannot create the directory %s", dir);
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    zb_info(opt, "downloading %llu files into %s", (unsigned long long)count,
            dir);

    for (i = 0; i < count; i++) {
        const zb_json *file = zb_json_at(files, i);
        const char *name = NULL;
        const char *file_token = NULL;
        uint64_t size = 0;
        char *safe_name = NULL;
        char *target = NULL;
        char *url = NULL;

        if (zb_interrupted) {
            zb_error_setf(err, ZB_ERR_CANCELED, "interrupted");
            rc = ZB_EXIT_INTERRUPTED;
            break;
        }
        if (!zb_json_get_str(file, "token", &file_token)) {
            continue;
        }
        (void)zb_json_get_str(file, "filename", &name);
        (void)zb_json_get_u64(file, "size", &size);

        safe_name = zb_sanitize_filename(name != NULL ? name : file_token);
        if (safe_name != NULL) {
            target = zb_path_join(dir, safe_name);
            url = download_url(opt, file_token);
        }
        if (safe_name == NULL || target == NULL || url == NULL) {
            zb_free(safe_name);
            zb_free(target);
            zb_free(url);
            zb_error_nomem(err);
            rc = zb_report_error(opt, err);
            break;
        }

        if (download_to_path(client, opt, url, target, size, safe_name,
                             args->force, err) != ZB_OK) {
            fprintf(stderr, "zhuzhbox: %s: %s\n", safe_name,
                    zb_error_message(err));
            rc = ZB_EXIT_ERROR;
            zb_error_clear(err);
        }
        zb_free(safe_name);
        zb_free(target);
        zb_free(url);
    }

cleanup:
    zb_free(dir);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Command                                                              */
/* ------------------------------------------------------------------ */

int zb_cmd_get(zb_options *opt, int argc, char **argv)
{
    get_args args;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    zb_client *client = NULL;
    zb_json *record = NULL;
    char *token = NULL;
    char *url = NULL;
    char *target = NULL;
    const char *type = NULL;
    int rc = ZB_EXIT_OK;

    memset(&args, 0, sizeof(args));
    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, on_option, &args, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count != 1) {
        zb_error_setf(&err, ZB_ERR_USAGE, "get takes exactly one link or token");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    token = zb_extract_token(positionals[0]);
    if (token == NULL) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "\"%s\" does not contain a zhuzhbox token",
                      positionals[0]);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    client = zb_client_new(opt, &err);
    if (client == NULL) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    if (zb_meta_get(client, token, &record, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    (void)zb_json_get_str(record, "type", &type);

    if (type != NULL && strcmp(type, "collection") == 0) {
        if (!args.all) {
            /* The CLI equivalent of the server-rendered collection page:
             * show what is in it, download nothing. */
            rc = opt->json ? zb_print_json(record) : list_collection(opt, record);
            goto cleanup;
        }
        rc = download_collection(client, opt, record, token, &args, &err);
        if (rc == ZB_EXIT_OK && opt->json) {
            rc = zb_print_json(record);
        }
        goto cleanup;
    }

    /* A single file. */
    {
        const char *name = NULL;
        uint64_t size = 0;
        char *safe_name = NULL;

        (void)zb_json_get_str(record, "filename", &name);
        (void)zb_json_get_u64(record, "size", &size);

        if (args.output != NULL && strcmp(args.output, "-") == 0) {
            target = NULL; /* stdout */
        } else if (args.output != NULL) {
            zb_stat_info info;
            /* -o pointing at a directory means "put it in there". */
            if (zb_stat(args.output, &info) == 0 && info.is_dir) {
                safe_name = zb_sanitize_filename(name != NULL ? name : token);
                target = safe_name != NULL ? zb_path_join(args.output, safe_name)
                                           : NULL;
            } else {
                target = zb_strdup(args.output);
            }
        } else {
            safe_name = zb_sanitize_filename(name != NULL ? name : token);
            target = zb_strdup(safe_name);
        }
        zb_free(safe_name);

        if (args.output == NULL || strcmp(args.output, "-") != 0) {
            if (target == NULL) {
                zb_error_nomem(&err);
                rc = zb_report_error(opt, &err);
                goto cleanup;
            }
        }

        url = download_url(opt, token);
        if (url == NULL) {
            zb_error_nomem(&err);
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }

        if (download_to_path(client, opt, url, target, size,
                             name != NULL ? name : token, args.force,
                             &err) != ZB_OK) {
            rc = zb_report_error(opt, &err);
            goto cleanup;
        }

        if (opt->json) {
            zb_json *out = zb_json_clone(record);
            if (out == NULL ||
                zb_json_obj_set_str(out, "savedTo",
                                    target != NULL ? target : "-") != 0) {
                zb_json_free(out);
                zb_error_nomem(&err);
                rc = zb_report_error(opt, &err);
                goto cleanup;
            }
            rc = zb_print_json(out);
            zb_json_free(out);
        } else if (target != NULL) {
            printf("%s\n", target);
        }
    }

cleanup:
    zb_free(target);
    zb_free(url);
    zb_free(token);
    zb_json_free(record);
    zb_client_free(client);
    zb_free(args.output);
    zb_free(positionals);
    zb_error_clear(&err);
    return rc;
}
