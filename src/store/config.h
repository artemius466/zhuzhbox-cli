/* config.h — layered configuration: flags > environment > config.json >
 * built-in defaults.
 *
 * Every setting records where its current value came from, so `config list`
 * can show it and so `config set` never silently writes back a value that was
 * really an environment override. */
#ifndef ZB_STORE_CONFIG_H
#define ZB_STORE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "util/error.h"

typedef enum {
    ZB_SRC_DEFAULT = 0,
    ZB_SRC_FILE,
    ZB_SRC_ENV,
    ZB_SRC_FLAG
} zb_config_source;

/* Tri-state settings use -1 for "auto", 0 for off, 1 for on. */
typedef struct {
    char *api_host;
    char *download_host;
    char *site_host;
    int concurrency;
    int bundle_default;  /* 0 separate uploads, 1 bundle */
    int color;           /* tri-state */
    int progress;        /* tri-state */
    int single_shot;     /* use POST /v1/sharex under the threshold */
    uint64_t single_shot_max;
    int resume_default;  /* auto-resume an interrupted upload without asking */

    zb_config_source src[10];
} zb_config;

/* Index into zb_config.src, in the same order the key table declares them. */
typedef enum {
    ZB_CFG_API_HOST = 0,
    ZB_CFG_DOWNLOAD_HOST,
    ZB_CFG_SITE_HOST,
    ZB_CFG_CONCURRENCY,
    ZB_CFG_BUNDLE,
    ZB_CFG_COLOR,
    ZB_CFG_PROGRESS,
    ZB_CFG_SINGLE_SHOT,
    ZB_CFG_SINGLE_SHOT_MAX,
    ZB_CFG_RESUME,
    ZB_CFG_COUNT
} zb_config_key;

/* Populate with built-in defaults. Always succeeds unless memory runs out. */
zb_status zb_config_defaults(zb_config *cfg, zb_error *err);

/* Overlay values from `base_dir`/config.json, then from the environment.
 * A missing config file is fine. A malformed one is an error naming the path —
 * we never silently ignore a file the user hand-edited. */
zb_status zb_config_load(zb_config *cfg, const char *base_dir, zb_error *err);

/* Write only the settings that came from the config file or were just set, so
 * saving does not bake an environment override into the file. */
zb_status zb_config_save(const zb_config *cfg, const char *base_dir,
                         zb_error *err);

void zb_config_free(zb_config *cfg);

/* ---------- key access, shared by `config get|set|list` ---------- */

size_t zb_config_key_count(void);
const char *zb_config_key_name(size_t index);
const char *zb_config_key_doc(size_t index);
const char *zb_config_key_env(size_t index);

/* Look up by name. Returns -1 when the key is unknown. */
int zb_config_key_index(const char *name);

/* Current value as text (owned; caller zb_free). NULL on OOM. */
char *zb_config_key_value(const zb_config *cfg, size_t index);

zb_config_source zb_config_key_source(const zb_config *cfg, size_t index);
const char *zb_config_source_name(zb_config_source src);

/* Parse and apply `text` to the key. Marks the source as ZB_SRC_FLAG so a
 * subsequent save persists it. */
zb_status zb_config_key_set(zb_config *cfg, size_t index, const char *text,
                            zb_error *err);

#endif /* ZB_STORE_CONFIG_H */
