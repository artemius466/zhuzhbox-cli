/* paths.h — the one place that knows where our files live.
 *
 * Linux:   $XDG_CONFIG_HOME/zhuzhbox, else ~/.config/zhuzhbox
 * macOS:   ~/Library/Application Support/zhuzhbox
 * Windows: %APPDATA%\zhuzhbox
 */
#ifndef ZB_STORE_PATHS_H
#define ZB_STORE_PATHS_H

/* Resolve the base directory. `override_dir` is --config or $ZHUZHBOX_CONFIG_DIR
 * and is used verbatim when non-NULL. Owned result, NULL if the home directory
 * cannot be determined. */
char *zb_paths_base_dir(const char *override_dir);

/* Create the base directory (0700 on POSIX) if it is missing. 0 on success. */
int zb_paths_ensure_dir(const char *base_dir);

/* base_dir + "/" + name. Owned, NULL on OOM. */
char *zb_paths_file(const char *base_dir, const char *name);

#define ZB_CONFIG_FILE_NAME "config.json"
#define ZB_SHELF_FILE_NAME "shelf.json"
#define ZB_SESSIONS_FILE_NAME "sessions.json"

#endif /* ZB_STORE_PATHS_H */
