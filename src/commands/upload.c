#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/client.h"
#include "api/collection.h"
#include "api/upload.h"
#include "commands/commands.h"
#include "format/bytes.h"
#include "format/color.h"
#include "format/time.h"
#include "zb_limits.h"
#include "store/shelf.h"
#include "util/buf.h"
#include "util/mime.h"
#include "util/platform.h"
#include "util/str.h"

void zb_help_upload(FILE *out)
{
    fputs(
        "By default each file becomes its own upload with its own link.\n"
        "With --bundle they all end up behind one collection link, and every\n"
        "member inherits the collection's expiry instead of its own\n"
        "size-based tier.\n"
        "\n"
        "Options:\n"
        "  -b, --bundle             upload everything as one collection\n"
        "      --title TEXT         collection title (max 200 characters)\n"
        "      --description TEXT   collection description (max 4000 chars)\n"
        "  -c, --concurrency N      files in flight at once (default 3)\n"
        "      --name NAME          filename to use, mainly for stdin\n"
        "      --resume             resume an interrupted upload without "
        "asking\n"
        "      --no-resume          always start over\n"
        "\n"
        "Reading from stdin:\n"
        "  `zhuzhbox upload -` buffers stdin to a private temporary file\n"
        "  first, because the API needs an exact size before the upload\n"
        "  starts and a pipe cannot be measured or rewound. The temp file is\n"
        "  created 0600 and removed on the way out, including on Ctrl+C.\n"
        "\n"
        "Limits enforced before anything touches the network:\n"
        "  25 GiB per file, 200 files per collection.\n"
        "\n"
        "Retention depends on size: under 5 GiB keeps for 30 days, 5 GiB and\n"
        "up 15 days, 15 GiB and up 7 days, 20 GiB and up 3 days.\n"
        "\n"
        "Every completed upload is written to the local shelf immediately,\n"
        "delete token included. That file is the only way to delete an upload\n"
        "later — there is no account to log into.\n",
        out);
}

typedef struct {
    zb_options *opt;
    int bundle;
    char *title;
    char *description;
    char *name_override;
    int concurrency;
    int resume_flag;    /* 1 = --resume */
    int no_resume_flag; /* 1 = --no-resume */
    zb_error *err;
} upload_args;

enum {
    OPT_BUNDLE = 1,
    OPT_TITLE,
    OPT_DESCRIPTION,
    OPT_CONCURRENCY,
    OPT_NAME,
    OPT_RESUME,
    OPT_NO_RESUME
};

static const zb_opt_spec k_specs[] = {
    {"bundle", 'b', ZB_ARG_NONE, NULL, "upload everything as one collection"},
    {"title", 0, ZB_ARG_REQUIRED, "TEXT", "collection title"},
    {"description", 0, ZB_ARG_REQUIRED, "TEXT", "collection description"},
    {"concurrency", 'c', ZB_ARG_REQUIRED, "N", "files in flight at once"},
    {"name", 0, ZB_ARG_REQUIRED, "NAME", "filename to use (mainly for stdin)"},
    {"resume", 0, ZB_ARG_NONE, NULL, "resume without asking"},
    {"no-resume", 0, ZB_ARG_NONE, NULL, "always start over"},
    {NULL, 0, ZB_ARG_NONE, NULL, NULL},
};

static zb_status take_string(char **slot, const char *value, zb_error *err)
{
    char *copy = zb_strdup(value);
    if (copy == NULL) {
        return zb_error_nomem(err);
    }
    zb_free(*slot);
    *slot = copy;
    return ZB_OK;
}

static zb_status on_option(void *ctx, const zb_opt_spec *spec,
                           const char *value, zb_error *err)
{
    upload_args *args = ctx;

    if (strcmp(spec->long_name, "bundle") == 0) {
        args->bundle = 1;
    } else if (strcmp(spec->long_name, "title") == 0) {
        return take_string(&args->title, value, err);
    } else if (strcmp(spec->long_name, "description") == 0) {
        return take_string(&args->description, value, err);
    } else if (strcmp(spec->long_name, "name") == 0) {
        return take_string(&args->name_override, value, err);
    } else if (strcmp(spec->long_name, "concurrency") == 0) {
        long n;
        char *end = NULL;
        n = strtol(value, &end, 10);
        if (end == value || *end != '\0' || n < 1 || n > ZB_MAX_CONCURRENCY) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "--concurrency must be between 1 and %d",
                                 ZB_MAX_CONCURRENCY);
        }
        args->concurrency = (int)n;
    } else if (strcmp(spec->long_name, "resume") == 0) {
        args->resume_flag = 1;
    } else if (strcmp(spec->long_name, "no-resume") == 0) {
        args->no_resume_flag = 1;
    }
    return ZB_OK;
}

/* ------------------------------------------------------------------ */
/* stdin                                                                */
/* ------------------------------------------------------------------ */

/* The API needs an exact size at init time and a pipe cannot be measured or
 * rewound, so stdin is spooled to a private temp file first. Guessing a size
 * or sending the wrong Content-Length would produce a silently truncated
 * upload, which is the worst thing this program could do (§5). */
static zb_status spool_stdin(char **path_out, uint64_t *size_out, zb_error *err)
{
    char *path = NULL;
    FILE *fp = NULL;
    char buffer[65536];
    size_t n;
    uint64_t total = 0;

    if (zb_temp_file("zhuzhbox-stdin", &path, &fp) != 0) {
        return zb_error_setf(err, ZB_ERR_IO,
                             "cannot create a temporary file to buffer stdin");
    }
    while ((n = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        if (fwrite(buffer, 1, n, fp) != n) {
            fclose(fp);
            zb_remove(path);
            zb_free(path);
            return zb_error_setf(err, ZB_ERR_IO,
                                 "cannot write stdin to the temporary file");
        }
        total += n;
        if (zb_interrupted) {
            fclose(fp);
            zb_remove(path);
            zb_free(path);
            return zb_error_setf(err, ZB_ERR_CANCELED, "interrupted");
        }
    }
    if (ferror(stdin)) {
        fclose(fp);
        zb_remove(path);
        zb_free(path);
        return zb_error_setf(err, ZB_ERR_IO, "cannot read stdin");
    }
    if (fflush(fp) != 0 || fclose(fp) != 0) {
        zb_remove(path);
        zb_free(path);
        return zb_error_setf(err, ZB_ERR_IO,
                             "cannot flush stdin to the temporary file");
    }

    *path_out = path;
    *size_out = total;
    return ZB_OK;
}

/* ------------------------------------------------------------------ */
/* Shelf write-through                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    zb_options *opt;
    int record_files; /* off in bundle mode: the collection is the shelf entry */
    int failed;
} shelf_writer;

static void record_result(zb_options *opt, const zb_json *result,
                          const char *kind, const char *fallback_name)
{
    zb_shelf_entry entry;
    zb_error err;
    const char *s = NULL;

    memset(&entry, 0, sizeof(entry));
    zb_error_init(&err);

    if (!zb_json_get_str(result, "token", &s)) {
        return;
    }
    entry.token = (char *)(uintptr_t)s; /* borrowed: upsert copies everything */
    entry.kind = (char *)(uintptr_t)kind;
    if (zb_json_get_str(result, "deleteToken", &s)) {
        entry.delete_token = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "filename", &s) ||
        zb_json_get_str(result, "title", &s)) {
        entry.name = (char *)(uintptr_t)s;
    } else {
        entry.name = (char *)(uintptr_t)fallback_name;
    }
    if (zb_json_get_str(result, "url", &s)) {
        entry.url = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "uploadedAt", &s)) {
        entry.uploaded_at = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "expiresAt", &s)) {
        entry.expires_at = (char *)(uintptr_t)s;
    }
    if (!zb_json_get_u64(result, "size", &entry.size)) {
        (void)zb_json_get_u64(result, "totalSize", &entry.size);
    }

    /* Written the moment the upload completes, not at the end of the run: a
     * crash one second later must not cost the delete token. */
    if (zb_shelf_record(opt->base_dir, &entry, &err) != ZB_OK) {
        fprintf(stderr,
                "zhuzhbox: the upload succeeded but the local record could not "
                "be saved: %s\n",
                zb_error_message(&err));
        if (entry.delete_token != NULL) {
            fprintf(stderr,
                    "zhuzhbox: write this down — delete token for %s is %s\n",
                    entry.token, entry.delete_token);
        }
    }
    zb_error_clear(&err);
}

static void on_upload_complete(void *ctx, size_t job_index,
                              const zb_json *result)
{
    shelf_writer *writer = ctx;
    (void)job_index;

    if (!writer->record_files || result == NULL) {
        return;
    }
    record_result(writer->opt, result, "file", NULL);
}

static int confirm_resume(void *ctx, const char *path, uint64_t bytes_done,
                          uint64_t total)
{
    zb_options *opt = ctx;
    char done_text[ZB_BYTES_BUF];
    char total_text[ZB_BYTES_BUF];
    zb_error err;
    int answer;

    zb_error_init(&err);
    zb_format_bytes(bytes_done, done_text, sizeof(done_text));
    zb_format_bytes(total, total_text, sizeof(total_text));

    answer = zb_confirm(opt, 0, &err,
                        "%s has an unfinished upload (%s of %s already sent). "
                        "Resume it?",
                        path, done_text, total_text);
    zb_error_clear(&err);
    /* No terminal to ask on means "start over", which is always safe. */
    return answer == 1;
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void print_upload_text(const zb_options *opt, const zb_json *result,
                              const char *kind)
{
    const char *bold = zb_color(opt->color, ZB_C_BOLD);
    const char *cyan = zb_color(opt->color, ZB_C_CYAN);
    const char *dim = zb_color(opt->color, ZB_C_DIM);
    const char *reset = zb_color(opt->color, ZB_C_RESET);
    const char *name = NULL;
    const char *url = NULL;
    const char *expires = NULL;
    uint64_t size = 0;
    char size_text[ZB_BYTES_BUF];
    char expiry_text[ZB_TIME_BUF];

    if (!zb_json_get_str(result, "filename", &name)) {
        (void)zb_json_get_str(result, "title", &name);
    }
    (void)zb_json_get_str(result, "url", &url);
    (void)zb_json_get_str(result, "expiresAt", &expires);
    if (!zb_json_get_u64(result, "size", &size)) {
        (void)zb_json_get_u64(result, "totalSize", &size);
    }
    zb_format_bytes(size, size_text, sizeof(size_text));

    expiry_text[0] = '\0';
    if (expires != NULL) {
        int64_t epoch = 0;
        if (zb_time_parse_iso8601(expires, &epoch) == 0) {
            zb_time_relative(epoch, zb_now_unix(), expiry_text,
                             sizeof(expiry_text));
        }
    }

    printf("%s%s%s%s\n", bold, name != NULL ? name : kind, reset,
           strcmp(kind, "collection") == 0 ? " (bundle)" : "");
    printf("  %s%s%s\n", cyan, url != NULL ? url : "(no url)", reset);
    printf("  %s%s", dim, size_text);
    if (expiry_text[0] != '\0') {
        printf(" · expires %s", expiry_text);
    }
    printf("%s\n", reset);
}

/* Merge the shelf view of an upload into its API result, so --json consumers
 * get one object with everything in it. Returns an owned copy. */
static zb_json *result_with_shelf(const zb_json *result, const char *kind)
{
    zb_json *copy = zb_json_clone(result);
    zb_shelf_entry entry;
    zb_json *shelf_json;
    const char *s = NULL;

    if (copy == NULL) {
        return NULL;
    }
    memset(&entry, 0, sizeof(entry));
    if (zb_json_get_str(result, "token", &s)) {
        entry.token = (char *)(uintptr_t)s;
    }
    entry.kind = (char *)(uintptr_t)kind;
    if (zb_json_get_str(result, "filename", &s) ||
        zb_json_get_str(result, "title", &s)) {
        entry.name = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "url", &s)) {
        entry.url = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "uploadedAt", &s)) {
        entry.uploaded_at = (char *)(uintptr_t)s;
    }
    if (zb_json_get_str(result, "expiresAt", &s)) {
        entry.expires_at = (char *)(uintptr_t)s;
    }
    if (!zb_json_get_u64(result, "size", &entry.size)) {
        (void)zb_json_get_u64(result, "totalSize", &entry.size);
    }

    /* The delete token is already in the result object the server sent; the
     * shelf view deliberately does not repeat it. */
    shelf_json = zb_shelf_entry_json(&entry, 0);
    if (shelf_json == NULL || zb_json_obj_add(copy, "shelfEntry", shelf_json) !=
                                  0) {
        zb_json_free(copy);
        return NULL;
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* Command                                                              */
/* ------------------------------------------------------------------ */

static int run_upload(zb_options *opt, upload_args *args, char **paths,
                      size_t path_count, zb_error *err);

int zb_cmd_upload(zb_options *opt, int argc, char **argv)
{
    upload_args args;
    zb_error err;
    char **positionals = NULL;
    size_t positional_count = 0;
    int rc = ZB_EXIT_OK;

    memset(&args, 0, sizeof(args));
    args.opt = opt;
    args.concurrency = opt->cfg.concurrency;
    args.bundle = opt->cfg.bundle_default;
    zb_error_init(&err);

    if (zb_parse_args(argc, argv, k_specs, on_option, &args, &positionals,
                      &positional_count, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (positional_count == 0) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "upload needs at least one file (use - to read stdin)");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (args.resume_flag && args.no_resume_flag) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "--resume and --no-resume contradict each other");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (!args.bundle && (args.title != NULL || args.description != NULL)) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "--title and --description only apply to --bundle");
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (args.bundle && positional_count > ZB_MAX_FILES_PER_COLLECTION) {
        zb_error_setf(&err, ZB_ERR_USAGE,
                      "a collection holds at most %d files; you passed %llu",
                      ZB_MAX_FILES_PER_COLLECTION,
                      (unsigned long long)positional_count);
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }
    if (zb_collection_validate(args.title, args.description, &err) != ZB_OK) {
        rc = zb_report_error(opt, &err);
        goto cleanup;
    }

    rc = run_upload(opt, &args, positionals, positional_count, &err);

cleanup:
    zb_free(positionals);
    zb_free(args.title);
    zb_free(args.description);
    zb_free(args.name_override);
    zb_error_clear(&err);
    return rc;
}

static int run_upload(zb_options *opt, upload_args *args, char **paths,
                      size_t path_count, zb_error *err)
{
    zb_client *client = NULL;
    zb_upload_engine *engine = NULL;
    zb_json *collection_init = NULL;
    zb_json *collection_result = NULL;
    zb_json *json_out = NULL;
    char *spooled_path = NULL;
    int spooled_engine_owns_file = 0;
    const char *collection_token = NULL;
    shelf_writer writer;
    zb_resume_mode resume;
    size_t i;
    int rc = ZB_EXIT_OK;
    int any_failed = 0;

    memset(&writer, 0, sizeof(writer));
    writer.opt = opt;
    writer.record_files = !args->bundle;

    resume = args->resume_flag      ? ZB_RESUME_ALWAYS
             : args->no_resume_flag ? ZB_RESUME_NEVER
             : opt->cfg.resume_default ? ZB_RESUME_ALWAYS
                                       : ZB_RESUME_ASK;

    client = zb_client_new(opt, err);
    if (client == NULL) {
        return zb_report_error(opt, err);
    }

    if (args->bundle) {
        if (zb_collection_init(client, args->title, args->description,
                               &collection_init, err) != ZB_OK) {
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        if (!zb_json_get_str(collection_init, "token", &collection_token)) {
            zb_error_setf(err, ZB_ERR_PROTO,
                          "the server did not return a collection token");
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        /* Put the collection on the shelf before uploading a single byte: its
         * delete token cascades to every member, so this one row is enough to
         * clean up even if the run dies halfway. */
        record_result(opt, collection_init, "collection",
                      args->title != NULL ? args->title : "bundle");
    }

    engine = zb_upload_engine_new(client, opt, opt->base_dir, collection_token,
                                  args->concurrency, resume, err);
    if (engine == NULL) {
        rc = zb_report_error(opt, err);
        goto cleanup;
    }
    zb_upload_engine_set_callback(engine, on_upload_complete, &writer);

    for (i = 0; i < path_count; i++) {
        const char *path = paths[i];
        char *filename = NULL;
        const char *mime;

        if (strcmp(path, "-") == 0) {
            uint64_t size = 0;
            if (spooled_path != NULL) {
                zb_error_setf(err, ZB_ERR_USAGE,
                              "stdin can only be uploaded once per run");
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
            zb_info(opt, "buffering stdin…");
            if (spool_stdin(&spooled_path, &size, err) != ZB_OK) {
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
            if (size == 0) {
                zb_error_setf(err, ZB_ERR_USAGE, "stdin was empty");
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
            path = spooled_path;
            filename = zb_sanitize_filename(args->name_override != NULL
                                                ? args->name_override
                                                : "stdin");
        } else {
            char *base = zb_path_basename(path);
            filename = zb_sanitize_filename(
                args->name_override != NULL && path_count == 1
                    ? args->name_override
                    : base);
            zb_free(base);
        }
        if (filename == NULL) {
            zb_error_nomem(err);
            rc = zb_report_error(opt, err);
            goto cleanup;
        }

        mime = zb_mime_from_path(filename);
        {
            /* The temp file behind stdin has to be removed by someone. The
             * engine copies the path string rather than taking it, so we keep
             * ownership of the string either way and only hand over
             * responsibility for unlinking the file. */
            int is_temp = spooled_path != NULL && path == spooled_path;
            zb_status add = zb_upload_engine_add(engine, path, filename, mime,
                                                 is_temp, err);
            zb_free(filename);
            if (add != ZB_OK) {
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
            if (is_temp) {
                spooled_engine_owns_file = 1;
            }
        }
    }

    zb_upload_engine_resolve_resume(engine, confirm_resume, opt);

    if (zb_upload_engine_run(engine, err) != ZB_OK) {
        if (err->status == ZB_ERR_CANCELED) {
            fputs("\nzhuzhbox: interrupted. Progress was saved — re-run the "
                  "same command with --resume to continue.\n",
                  stderr);
            rc = ZB_EXIT_INTERRUPTED;
            goto cleanup;
        }
        any_failed = 1;
    }

    /* Report per-file failures individually: one bad file in a batch of ten
     * should not hide the nine that worked. */
    for (i = 0; i < zb_upload_count(engine); i++) {
        if (zb_upload_job_status(engine, i) != ZB_OK) {
            const zb_error *job_err = zb_upload_job_error(engine, i);
            any_failed = 1;
            fprintf(stderr, "zhuzhbox: %s: %s\n",
                    zb_upload_job_filename(engine, i),
                    zb_error_message(job_err));
        }
    }

    if (args->bundle) {
        size_t succeeded = 0;
        for (i = 0; i < zb_upload_count(engine); i++) {
            if (zb_upload_job_status(engine, i) == ZB_OK) {
                succeeded++;
            }
        }
        if (succeeded == 0) {
            zb_error_setf(err, ZB_ERR_HTTP,
                          "no file made it into the bundle, so there is "
                          "nothing to seal");
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        if (zb_collection_complete(client, collection_token, &collection_result,
                                   err) != ZB_OK) {
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        /* Now that it is sealed, the shelf row gets the real title, size and
         * expiry. The delete token from init is preserved by the upsert only
         * if the completion response repeats it, so pass it through. */
        {
            const char *delete_token = NULL;
            if (!zb_json_get_str(collection_result, "deleteToken",
                                 &delete_token) &&
                zb_json_get_str(collection_init, "deleteToken",
                                &delete_token)) {
                (void)zb_json_obj_set_str(collection_result, "deleteToken",
                                          delete_token);
            }
        }
        record_result(opt, collection_result, "collection",
                      args->title != NULL ? args->title : "bundle");
    }

    /* ---- output ---- */
    if (opt->json) {
        json_out = zb_json_new_object();
        if (json_out == NULL) {
            zb_error_nomem(err);
            rc = zb_report_error(opt, err);
            goto cleanup;
        }
        if (args->bundle) {
            zb_json *merged = result_with_shelf(collection_result, "collection");
            if (merged == NULL ||
                zb_json_obj_set_str(json_out, "mode", "bundle") != 0 ||
                zb_json_obj_add(json_out, "collection", merged) != 0) {
                zb_error_nomem(err);
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
        } else {
            zb_json *array = zb_json_new_array();
            if (array == NULL ||
                zb_json_obj_set_str(json_out, "mode", "separate") != 0) {
                zb_json_free(array);
                zb_error_nomem(err);
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
            for (i = 0; i < zb_upload_count(engine); i++) {
                const zb_json *result = zb_upload_job_result(engine, i);
                zb_json *merged;
                if (result == NULL) {
                    continue;
                }
                merged = result_with_shelf(result, "file");
                if (merged == NULL || zb_json_arr_add(array, merged) != 0) {
                    zb_json_free(array);
                    zb_error_nomem(err);
                    rc = zb_report_error(opt, err);
                    goto cleanup;
                }
            }
            if (zb_json_obj_add(json_out, "uploads", array) != 0) {
                zb_error_nomem(err);
                rc = zb_report_error(opt, err);
                goto cleanup;
            }
        }
        if (zb_print_json(json_out) != ZB_EXIT_OK) {
            rc = ZB_EXIT_ERROR;
            goto cleanup;
        }
    } else if (args->bundle) {
        print_upload_text(opt, collection_result, "collection");
    } else {
        for (i = 0; i < zb_upload_count(engine); i++) {
            const zb_json *result = zb_upload_job_result(engine, i);
            if (result != NULL) {
                print_upload_text(opt, result, "file");
            }
        }
    }

    if (any_failed && rc == ZB_EXIT_OK) {
        rc = ZB_EXIT_ERROR;
    }

cleanup:
    zb_json_free(json_out);
    zb_json_free(collection_result);
    zb_json_free(collection_init);
    zb_upload_engine_free(engine);
    zb_client_free(client);
    if (spooled_path != NULL) {
        /* If the engine never took the file on, it is still ours to unlink —
         * a temp file holding piped data must not outlive the process. */
        if (!spooled_engine_owns_file) {
            zb_remove(spooled_path);
        }
        zb_free(spooled_path);
    }
    return rc;
}
