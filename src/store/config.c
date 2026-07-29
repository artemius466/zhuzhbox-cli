#include "store/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "format/bytes.h"
#include "zb_limits.h"
#include "store/atomic.h"
#include "store/paths.h"
#include "util/buf.h"
#include "util/json.h"
#include "util/platform.h"
#include "util/str.h"

typedef enum { CFG_STR, CFG_INT, CFG_BOOL, CFG_TRI, CFG_SIZE } cfg_type;

struct cfg_desc {
    const char *name;
    cfg_type type;
    const char *env;
    const char *doc;
};

/* Order must match zb_config_key. */
static const struct cfg_desc k_keys[ZB_CFG_COUNT] = {
    {"apiHost", CFG_STR, "ZHUZHBOX_API_HOST", "base URL of the API"},
    {"downloadHost", CFG_STR, "ZHUZHBOX_DOWNLOAD_HOST",
     "base URL that serves /d/<token>"},
    {"siteHost", CFG_STR, "ZHUZHBOX_SITE_HOST",
     "base URL of the website, used when printing share links"},
    {"concurrency", CFG_INT, "ZHUZHBOX_CONCURRENCY",
     "files uploaded at once (1-16)"},
    {"bundle", CFG_BOOL, "ZHUZHBOX_BUNDLE",
     "upload multiple files as one collection by default"},
    {"color", CFG_TRI, "ZHUZHBOX_COLOR", "auto | on | off"},
    {"progress", CFG_TRI, "ZHUZHBOX_PROGRESS", "auto | on | off"},
    {"singleShot", CFG_BOOL, "ZHUZHBOX_SINGLE_SHOT",
     "use the one-request upload path for small files"},
    {"singleShotMax", CFG_SIZE, "ZHUZHBOX_SINGLE_SHOT_MAX",
     "size below which the one-request path is used"},
    {"resume", CFG_BOOL, "ZHUZHBOX_RESUME",
     "resume an interrupted upload without asking"},
};

static char **str_field(zb_config *cfg, size_t index)
{
    switch ((zb_config_key)index) {
    case ZB_CFG_API_HOST:
        return &cfg->api_host;
    case ZB_CFG_DOWNLOAD_HOST:
        return &cfg->download_host;
    case ZB_CFG_SITE_HOST:
        return &cfg->site_host;
    default:
        return NULL;
    }
}

static int *int_field(zb_config *cfg, size_t index)
{
    switch ((zb_config_key)index) {
    case ZB_CFG_CONCURRENCY:
        return &cfg->concurrency;
    case ZB_CFG_BUNDLE:
        return &cfg->bundle_default;
    case ZB_CFG_COLOR:
        return &cfg->color;
    case ZB_CFG_PROGRESS:
        return &cfg->progress;
    case ZB_CFG_SINGLE_SHOT:
        return &cfg->single_shot;
    case ZB_CFG_RESUME:
        return &cfg->resume_default;
    default:
        return NULL;
    }
}

/* Read-only counterparts, so nothing has to cast away const to print a value. */
static const char *str_value(const zb_config *cfg, size_t index)
{
    switch ((zb_config_key)index) {
    case ZB_CFG_API_HOST:
        return cfg->api_host != NULL ? cfg->api_host : "";
    case ZB_CFG_DOWNLOAD_HOST:
        return cfg->download_host != NULL ? cfg->download_host : "";
    case ZB_CFG_SITE_HOST:
        return cfg->site_host != NULL ? cfg->site_host : "";
    default:
        return "";
    }
}

static int int_value(const zb_config *cfg, size_t index)
{
    switch ((zb_config_key)index) {
    case ZB_CFG_CONCURRENCY:
        return cfg->concurrency;
    case ZB_CFG_BUNDLE:
        return cfg->bundle_default;
    case ZB_CFG_COLOR:
        return cfg->color;
    case ZB_CFG_PROGRESS:
        return cfg->progress;
    case ZB_CFG_SINGLE_SHOT:
        return cfg->single_shot;
    case ZB_CFG_RESUME:
        return cfg->resume_default;
    default:
        return 0;
    }
}

size_t zb_config_key_count(void)
{
    return ZB_CFG_COUNT;
}

const char *zb_config_key_name(size_t index)
{
    return index < ZB_CFG_COUNT ? k_keys[index].name : NULL;
}

const char *zb_config_key_doc(size_t index)
{
    return index < ZB_CFG_COUNT ? k_keys[index].doc : NULL;
}

const char *zb_config_key_env(size_t index)
{
    return index < ZB_CFG_COUNT ? k_keys[index].env : NULL;
}

int zb_config_key_index(const char *name)
{
    size_t i;
    if (name == NULL) {
        return -1;
    }
    for (i = 0; i < ZB_CFG_COUNT; i++) {
        if (zb_streq_ci(name, k_keys[i].name)) {
            return (int)i;
        }
    }
    return -1;
}

zb_config_source zb_config_key_source(const zb_config *cfg, size_t index)
{
    return index < ZB_CFG_COUNT ? cfg->src[index] : ZB_SRC_DEFAULT;
}

const char *zb_config_source_name(zb_config_source src)
{
    switch (src) {
    case ZB_SRC_DEFAULT:
        return "default";
    case ZB_SRC_FILE:
        return "config file";
    case ZB_SRC_ENV:
        return "environment";
    case ZB_SRC_FLAG:
        return "flag";
    }
    return "default";
}

/* Strip a trailing slash so path concatenation never produces "//v1". */
static char *normalize_host(const char *value)
{
    size_t len;
    char *copy = zb_strdup(value);
    if (copy == NULL) {
        return NULL;
    }
    len = strlen(copy);
    while (len > 0 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    return copy;
}

static zb_status set_str(zb_config *cfg, size_t index, const char *value,
                         zb_config_source src, zb_error *err)
{
    char **slot = str_field(cfg, index);
    char *copy;

    if (slot == NULL) {
        return zb_error_setf(err, ZB_ERR_USAGE, "not a text setting");
    }
    if (value == NULL || value[0] == '\0') {
        return zb_error_setf(err, ZB_ERR_USAGE, "%s cannot be empty",
                             k_keys[index].name);
    }
    copy = normalize_host(value);
    if (copy == NULL) {
        return zb_error_nomem(err);
    }
    zb_free(*slot);
    *slot = copy;
    cfg->src[index] = src;
    return ZB_OK;
}

static int parse_tri(const char *s, int *out)
{
    if (zb_streq_ci(s, "auto")) {
        *out = -1;
    } else if (zb_streq_ci(s, "on") || zb_streq_ci(s, "yes") ||
               zb_streq_ci(s, "true") || zb_streq_ci(s, "1") ||
               zb_streq_ci(s, "always")) {
        *out = 1;
    } else if (zb_streq_ci(s, "off") || zb_streq_ci(s, "no") ||
               zb_streq_ci(s, "false") || zb_streq_ci(s, "0") ||
               zb_streq_ci(s, "never")) {
        *out = 0;
    } else {
        return -1;
    }
    return 0;
}

static int parse_bool(const char *s, int *out)
{
    int v;
    if (parse_tri(s, &v) != 0 || v < 0) {
        return -1;
    }
    *out = v;
    return 0;
}

static zb_status set_from_text(zb_config *cfg, size_t index, const char *text,
                               zb_config_source src, zb_error *err)
{
    int *slot;
    int value = 0;

    if (index >= ZB_CFG_COUNT) {
        return zb_error_setf(err, ZB_ERR_USAGE, "unknown setting");
    }

    switch (k_keys[index].type) {
    case CFG_STR:
        return set_str(cfg, index, text, src, err);

    case CFG_INT: {
        long n;
        char *end = NULL;
        n = strtol(text, &end, 10);
        if (end == text || (end != NULL && *end != '\0')) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "%s must be a number, got \"%s\"",
                                 k_keys[index].name, text);
        }
        if (index == ZB_CFG_CONCURRENCY &&
            (n < 1 || n > ZB_MAX_CONCURRENCY)) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "concurrency must be between 1 and %d",
                                 ZB_MAX_CONCURRENCY);
        }
        value = (int)n;
        break;
    }

    case CFG_BOOL:
        if (parse_bool(text, &value) != 0) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "%s must be on or off, got \"%s\"",
                                 k_keys[index].name, text);
        }
        break;

    case CFG_TRI:
        if (parse_tri(text, &value) != 0) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "%s must be auto, on or off, got \"%s\"",
                                 k_keys[index].name, text);
        }
        break;

    case CFG_SIZE: {
        uint64_t bytes;
        if (zb_parse_size(text, &bytes) != 0) {
            return zb_error_setf(err, ZB_ERR_USAGE,
                                 "%s must be a size like 20M, got \"%s\"",
                                 k_keys[index].name, text);
        }
        cfg->single_shot_max = bytes;
        cfg->src[index] = src;
        return ZB_OK;
    }
    }

    slot = int_field(cfg, index);
    if (slot == NULL) {
        return zb_error_setf(err, ZB_ERR_USAGE, "unknown setting");
    }
    *slot = value;
    cfg->src[index] = src;
    return ZB_OK;
}

zb_status zb_config_key_set(zb_config *cfg, size_t index, const char *text,
                            zb_error *err)
{
    return set_from_text(cfg, index, text, ZB_SRC_FLAG, err);
}

char *zb_config_key_value(const zb_config *cfg, size_t index)
{
    char buf[ZB_BYTES_BUF];

    if (index >= ZB_CFG_COUNT) {
        return NULL;
    }
    switch (k_keys[index].type) {
    case CFG_STR:
        return zb_strdup(str_value(cfg, index));
    case CFG_INT:
        return zb_asprintf("%d", int_value(cfg, index));
    case CFG_BOOL:
        return zb_strdup(int_value(cfg, index) ? "on" : "off");
    case CFG_TRI: {
        int v = int_value(cfg, index);
        return zb_strdup(v < 0 ? "auto" : (v ? "on" : "off"));
    }
    case CFG_SIZE:
        zb_format_bytes(cfg->single_shot_max, buf, sizeof(buf));
        return zb_strdup(buf);
    }
    return NULL;
}

zb_status zb_config_defaults(zb_config *cfg, zb_error *err)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->api_host = zb_strdup(ZB_DEFAULT_API_HOST);
    cfg->download_host = zb_strdup(ZB_DEFAULT_DOWNLOAD_HOST);
    cfg->site_host = zb_strdup(ZB_DEFAULT_SITE_HOST);
    if (cfg->api_host == NULL || cfg->download_host == NULL ||
        cfg->site_host == NULL) {
        zb_config_free(cfg);
        return zb_error_nomem(err);
    }
    cfg->concurrency = ZB_DEFAULT_CONCURRENCY;
    cfg->bundle_default = 0;
    cfg->color = -1;
    cfg->progress = -1;
    cfg->single_shot = 1;
    cfg->single_shot_max = ZB_DEFAULT_CHUNK_SIZE;
    cfg->resume_default = 0;
    return ZB_OK;
}

void zb_config_free(zb_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    zb_free(cfg->api_host);
    zb_free(cfg->download_host);
    zb_free(cfg->site_host);
    cfg->api_host = NULL;
    cfg->download_host = NULL;
    cfg->site_host = NULL;
}

static zb_status load_file(zb_config *cfg, const char *path, zb_error *err)
{
    char *text = NULL;
    size_t len = 0;
    zb_json *root = NULL;
    const zb_json *defaults;
    size_t i;
    zb_status rc;

    rc = zb_read_file(path, &text, &len, err);
    if (rc != ZB_OK) {
        return rc;
    }
    if (text == NULL || len == 0) {
        zb_free(text);
        return ZB_OK;
    }

    root = zb_json_parse(text, len);
    zb_free(text);
    if (root == NULL || !zb_json_is_object(root)) {
        zb_json_free(root);
        return zb_error_setf(err, ZB_ERR_IO,
                             "%s is not valid JSON — fix or remove it (it was "
                             "left untouched)",
                             path);
    }

    /* Settings live at the top level; the historic "defaults" object is still
     * honored so an older config file keeps working. */
    defaults = zb_json_get(root, "defaults");
    for (i = 0; i < ZB_CFG_COUNT; i++) {
        const zb_json *node = zb_json_get(root, k_keys[i].name);
        char scratch[64];
        const char *text_value = NULL;

        if (node == NULL && defaults != NULL) {
            node = zb_json_get(defaults, k_keys[i].name);
        }
        if (node == NULL || zb_json_is_null(node)) {
            continue;
        }

        if (zb_json_is_string(node)) {
            text_value = zb_json_str(node);
        } else if (zb_json_is_bool(node)) {
            int b = 0;
            if (zb_json_as_bool(node, &b)) {
                text_value = b ? "on" : "off";
            }
        } else if (zb_json_is_number(node)) {
            uint64_t n = 0;
            if (!zb_json_as_u64(node, &n)) {
                zb_json_free(root);
                return zb_error_setf(err, ZB_ERR_IO,
                                     "%s: %s is not a usable number", path,
                                     k_keys[i].name);
            }
            (void)snprintf(scratch, sizeof(scratch), "%llu",
                           (unsigned long long)n);
            text_value = scratch;
        }

        if (text_value == NULL) {
            zb_json_free(root);
            return zb_error_setf(err, ZB_ERR_IO, "%s: %s has an unusable value",
                                 path, k_keys[i].name);
        }
        rc = set_from_text(cfg, i, text_value, ZB_SRC_FILE, err);
        if (rc != ZB_OK) {
            zb_json_free(root);
            return rc;
        }
    }

    zb_json_free(root);
    return ZB_OK;
}

static zb_status load_env(zb_config *cfg, zb_error *err)
{
    size_t i;

    for (i = 0; i < ZB_CFG_COUNT; i++) {
        char *value = zb_getenv_dup(k_keys[i].env);
        zb_status rc;
        if (value == NULL) {
            continue;
        }
        rc = set_from_text(cfg, i, value, ZB_SRC_ENV, err);
        zb_free(value);
        if (rc != ZB_OK) {
            return rc;
        }
    }

    /* Accepted as a convenience alias for the API base URL. */
    {
        char *legacy = zb_getenv_dup("ZHUZHBOX_API");
        if (legacy != NULL) {
            zb_status rc = ZB_OK;
            if (cfg->src[ZB_CFG_API_HOST] != ZB_SRC_ENV) {
                rc = set_from_text(cfg, ZB_CFG_API_HOST, legacy, ZB_SRC_ENV,
                                   err);
            }
            zb_free(legacy);
            if (rc != ZB_OK) {
                return rc;
            }
        }
    }
    return ZB_OK;
}

zb_status zb_config_load(zb_config *cfg, const char *base_dir, zb_error *err)
{
    char *path;
    zb_status rc;

    if (base_dir != NULL) {
        path = zb_paths_file(base_dir, ZB_CONFIG_FILE_NAME);
        if (path == NULL) {
            return zb_error_nomem(err);
        }
        rc = load_file(cfg, path, err);
        zb_free(path);
        if (rc != ZB_OK) {
            return rc;
        }
    }
    return load_env(cfg, err);
}

zb_status zb_config_save(const zb_config *cfg, const char *base_dir,
                         zb_error *err)
{
    zb_json *root;
    char *path = NULL;
    char *text = NULL;
    size_t i;
    zb_status rc = ZB_OK;

    root = zb_json_new_object();
    if (root == NULL) {
        return zb_error_nomem(err);
    }

    for (i = 0; i < ZB_CFG_COUNT; i++) {
        int failed = 0;

        /* Only persist what the file already owned or what was just set —
         * writing back an env override would make it permanent by surprise. */
        if (cfg->src[i] != ZB_SRC_FILE && cfg->src[i] != ZB_SRC_FLAG) {
            continue;
        }
        switch (k_keys[i].type) {
        case CFG_STR:
            failed = zb_json_obj_set_str(root, k_keys[i].name,
                                         str_value(cfg, i)) != 0;
            break;
        case CFG_INT:
            failed = zb_json_obj_set_i64(root, k_keys[i].name,
                                         int_value(cfg, i)) != 0;
            break;
        case CFG_BOOL:
            failed = zb_json_obj_set_bool(root, k_keys[i].name,
                                          int_value(cfg, i)) != 0;
            break;
        case CFG_TRI: {
            int v = int_value(cfg, i);
            failed = zb_json_obj_set_str(root, k_keys[i].name,
                                         v < 0 ? "auto" : (v ? "on" : "off")) !=
                     0;
            break;
        }
        case CFG_SIZE:
            failed = zb_json_obj_set_u64(root, k_keys[i].name,
                                         cfg->single_shot_max) != 0;
            break;
        }
        if (failed) {
            rc = zb_error_nomem(err);
            goto cleanup;
        }
    }

    text = zb_json_print(root, 1);
    if (text == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    path = zb_paths_file(base_dir, ZB_CONFIG_FILE_NAME);
    if (path == NULL) {
        rc = zb_error_nomem(err);
        goto cleanup;
    }
    rc = zb_atomic_write(path, text, strlen(text), err);

cleanup:
    zb_free(path);
    zb_free(text);
    zb_json_free(root);
    return rc;
}
