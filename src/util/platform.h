/* platform.h — every OS-specific call in the tree lives behind this header.
 *
 * Rule from the spec: `#ifdef _WIN32` appears in platform.c/platform.h and
 * nowhere else. If you need a syscall, add it here rather than reaching for a
 * conditional in a command file.
 *
 * All `const char *path` arguments are UTF-8. On Windows they are converted to
 * UTF-16 and passed to the wide Win32 APIs, so a filename that has no
 * representation in the active ANSI code page still works.
 */
#ifndef ZB_UTIL_PLATFORM_H
#define ZB_UTIL_PLATFORM_H

#include <signal.h>
#include <stdint.h>
#include <stdio.h>

/* ---------- process-wide interrupt flag (§2.1) ----------
 * Set by the SIGINT/SIGTERM handler (or the Windows console control handler),
 * which does nothing else. Polled by the libcurl progress callback. */
extern volatile sig_atomic_t zb_interrupted;

void zb_install_signal_handlers(void);

/* ---------- console ---------- */

/* Set the console to UTF-8 and enable ANSI escape processing. Safe to call
 * once at startup on every platform; a failure just means no color. */
void zb_console_init(void);

int zb_isatty_stdout(void);
int zb_isatty_stderr(void);
int zb_isatty_stdin(void);

/* Terminal width in columns, or 80 if it cannot be determined. */
int zb_term_width(void);

/* Put stdout into binary mode so `get -o -` does not translate newlines. */
void zb_stdout_binary(void);

/* ---------- environment and directories ---------- */

/* Owned copy of an environment variable, or NULL if unset or empty. */
char *zb_getenv_dup(const char *name);

/* The user's home directory (POSIX) — $HOME, falling back to getpwuid().
 * Owned, NULL if it cannot be determined. */
char *zb_home_dir(void);

/* The directory into which per-application config directories belong:
 * %APPDATA% on Windows, ~/Library/Application Support on macOS, and
 * $XDG_CONFIG_HOME or ~/.config elsewhere. Owned, NULL if undeterminable.
 *
 * store/paths.c appends the application name; nothing else should call this. */
char *zb_user_config_root(void);

/* ---------- paths ---------- */

int zb_is_path_sep(char c);

/* Join two path components with the native separator. Owned, NULL on OOM. */
char *zb_path_join(const char *a, const char *b);

/* The directory part of `path`, or "." if there is none. Owned. */
char *zb_path_dirname(const char *path);

/* The final component of `path`. Owned. */
char *zb_path_basename(const char *path);

/* Create `path` and any missing parents, mode 0700 on POSIX. 0 on success. */
int zb_mkdir_p(const char *path);

/* ---------- files ---------- */

typedef struct {
    uint64_t size;
    int64_t mtime; /* seconds since the Unix epoch */
    int is_dir;
    int is_regular;
} zb_stat_info;

/* 0 on success, -1 if the path does not exist or cannot be stat'ed. Always
 * 64-bit, including on 32-bit builds. */
int zb_stat(const char *path, zb_stat_info *out);

int zb_path_exists(const char *path);

/* fopen with a UTF-8 path. `mode` is the usual C mode string; "b" is added
 * automatically where it matters. */
FILE *zb_fopen(const char *path, const char *mode);

/* Create a new file for writing with owner-only permissions, failing if it
 * already exists. This is how every file that may hold a delete token is
 * created — the mode is right from the moment the file exists, with no
 * create-then-chmod window and no dependence on umask. */
FILE *zb_fopen_private_new(const char *path);

/* 64-bit seek/tell regardless of the platform's off_t. */
int zb_fseek64(FILE *fp, int64_t offset, int whence);
int64_t zb_ftell64(FILE *fp);

/* Flush userspace buffers and force the file's data to stable storage. */
int zb_fsync(FILE *fp);

int zb_remove(const char *path);

/* Rename `from` over `to`, replacing `to` if it exists. Plain rename() cannot
 * do this on Windows, so this wraps MoveFileExW there. */
int zb_rename_replace(const char *from, const char *to);

/* Create a uniquely named 0600 file in the platform temp directory.
 * On success stores an owned path in *path_out and an open "w+b" stream in
 * *fp_out, and returns 0. */
int zb_temp_file(const char *prefix, char **path_out, FILE **fp_out);

/* ---------- time and sleep ---------- */

/* Milliseconds from an unspecified but monotonic origin — for measuring
 * elapsed time only. */
uint64_t zb_now_ms(void);

/* Wall-clock seconds since the Unix epoch. */
int64_t zb_now_unix(void);

/* Convert a UTC broken-down time to epoch seconds. Exists because timegm() is
 * not portable. `mon` is 1-12, `mday` is 1-31. */
int64_t zb_timegm_utc(int year, int mon, int mday, int hour, int min, int sec);

/* Convert epoch seconds back to UTC fields. */
void zb_gmtime_utc(int64_t t, int *year, int *mon, int *mday, int *hour,
                   int *min, int *sec);

void zb_sleep_ms(unsigned ms);

#endif /* ZB_UTIL_PLATFORM_H */
