/* platform.c — the only implementation file with #ifdef _WIN32. */

#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#include "util/platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/buf.h"

#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <shlobj.h>
#include <sys/stat.h>
#include <sys/types.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

volatile sig_atomic_t zb_interrupted = 0;

/* ------------------------------------------------------------------ */
/* Windows UTF-8 <-> UTF-16 helpers                                     */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/* Owned wide string, NULL on failure. */
static wchar_t *widen(const char *s)
{
    int n;
    wchar_t *w;
    if (s == NULL) {
        return NULL;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) {
        return NULL;
    }
    w = zb_malloc((size_t)n * sizeof(wchar_t));
    if (w == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        zb_free(w);
        return NULL;
    }
    return w;
}

/* Owned UTF-8 string, NULL on failure. */
static char *narrow(const wchar_t *w)
{
    int n;
    char *s;
    if (w == NULL) {
        return NULL;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    s = zb_malloc((size_t)n);
    if (s == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        zb_free(s);
        return NULL;
    }
    return s;
}
#endif

/* ------------------------------------------------------------------ */
/* Signals                                                              */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        zb_interrupted = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static void on_signal(int sig)
{
    (void)sig;
    /* Nothing but the flag: no malloc, no printf, no file I/O. */
    zb_interrupted = 1;
}
#endif

void zb_install_signal_handlers(void)
{
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: let blocking calls return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* A closed pipe (`zhuzhbox ls | head`) must not kill the process before
     * the shelf is flushed. */
    signal(SIGPIPE, SIG_IGN);
#endif
}

/* ------------------------------------------------------------------ */
/* Console                                                              */
/* ------------------------------------------------------------------ */

void zb_console_init(void)
{
#ifdef _WIN32
    HANDLE h;
    DWORD mode;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        /* If this fails we simply never emit ANSI — see zb_color_enabled(). */
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif
}

int zb_isatty_stdout(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

int zb_isatty_stderr(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

int zb_isatty_stdin(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

int zb_term_width(void)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) {
        int w = info.srWindow.Right - info.srWindow.Left + 1;
        if (w > 10) {
            return w;
        }
    }
    return 80;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10) {
        return (int)ws.ws_col;
    }
    return 80;
#endif
}

void zb_stdout_binary(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

/* ------------------------------------------------------------------ */
/* Environment and well-known directories                               */
/* ------------------------------------------------------------------ */

char *zb_getenv_dup(const char *name)
{
#ifdef _WIN32
    wchar_t *wname;
    wchar_t *value;
    DWORD n;
    char *out;

    wname = widen(name);
    if (wname == NULL) {
        return NULL;
    }
    n = GetEnvironmentVariableW(wname, NULL, 0);
    if (n == 0) {
        zb_free(wname);
        return NULL;
    }
    value = zb_malloc((size_t)n * sizeof(wchar_t));
    if (value == NULL) {
        zb_free(wname);
        return NULL;
    }
    if (GetEnvironmentVariableW(wname, value, n) == 0) {
        zb_free(wname);
        zb_free(value);
        return NULL;
    }
    zb_free(wname);
    out = narrow(value);
    zb_free(value);
    if (out != NULL && out[0] == '\0') {
        zb_free(out);
        return NULL;
    }
    return out;
#else
    const char *v = getenv(name);
    if (v == NULL || v[0] == '\0') {
        return NULL;
    }
    return zb_strdup(v);
#endif
}

char *zb_home_dir(void)
{
#ifdef _WIN32
    char *h = zb_getenv_dup("USERPROFILE");
    return h;
#else
    char *h = zb_getenv_dup("HOME");
    if (h != NULL) {
        return h;
    }
    {
        /* Never fall back to a hardcoded "~" — ask the password database. */
        struct passwd *pw = getpwuid(getuid());
        if (pw != NULL && pw->pw_dir != NULL && pw->pw_dir[0] != '\0') {
            return zb_strdup(pw->pw_dir);
        }
    }
    return NULL;
#endif
}

char *zb_user_config_root(void)
{
#ifdef _WIN32
    char *v = zb_getenv_dup("APPDATA");
    PWSTR wpath = NULL;
    char *out;

    if (v != NULL) {
        return v;
    }
    if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &wpath) != S_OK) {
        return NULL;
    }
    out = narrow(wpath);
    CoTaskMemFree(wpath);
    return out;
#else
    char *home;
    char *out;

#ifndef __APPLE__
    /* XDG only applies to the Unix-y platforms; macOS has its own convention. */
    out = zb_getenv_dup("XDG_CONFIG_HOME");
    if (out != NULL) {
        return out;
    }
#endif

    home = zb_home_dir();
    if (home == NULL) {
        return NULL;
    }
#ifdef __APPLE__
    out = zb_path_join(home, "Library/Application Support");
#else
    out = zb_path_join(home, ".config");
#endif
    zb_free(home);
    return out;
#endif
}

/* ------------------------------------------------------------------ */
/* Paths                                                                */
/* ------------------------------------------------------------------ */

int zb_is_path_sep(char c)
{
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

#ifdef _WIN32
#define ZB_PATH_SEP_CHAR '\\'
#else
#define ZB_PATH_SEP_CHAR '/'
#endif

char *zb_path_join(const char *a, const char *b)
{
    size_t la;

    if (a == NULL || a[0] == '\0') {
        return zb_strdup(b);
    }
    if (b == NULL || b[0] == '\0') {
        return zb_strdup(a);
    }
    la = strlen(a);
    if (zb_is_path_sep(a[la - 1])) {
        return zb_asprintf("%s%s", a, b);
    }
    return zb_asprintf("%s%c%s", a, ZB_PATH_SEP_CHAR, b);
}

char *zb_path_dirname(const char *path)
{
    size_t i;
    size_t cut = 0;
    int found = 0;

    if (path == NULL) {
        return NULL;
    }
    for (i = 0; path[i] != '\0'; i++) {
        if (zb_is_path_sep(path[i])) {
            cut = i;
            found = 1;
        }
    }
    if (!found) {
        return zb_strdup(".");
    }
    if (cut == 0) {
        /* "/foo" -> "/" */
        char root[2];
        root[0] = path[0];
        root[1] = '\0';
        return zb_strdup(root);
    }
    return zb_strndup(path, cut);
}

char *zb_path_basename(const char *path)
{
    const char *base;
    size_t i;

    if (path == NULL) {
        return NULL;
    }
    base = path;
    for (i = 0; path[i] != '\0'; i++) {
        if (zb_is_path_sep(path[i])) {
            base = path + i + 1;
        }
    }
    return zb_strdup(base);
}

static int mkdir_one(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    int rc;
    if (w == NULL) {
        return -1;
    }
    if (CreateDirectoryW(w, NULL)) {
        rc = 0;
    } else {
        rc = (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
    }
    zb_free(w);
    return rc;
#else
    if (mkdir(path, 0700) == 0) {
        return 0;
    }
    return errno == EEXIST ? 0 : -1;
#endif
}

int zb_mkdir_p(const char *path)
{
    char *work;
    size_t i;
    int rc = 0;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    work = zb_strdup(path);
    if (work == NULL) {
        return -1;
    }
    for (i = 1; work[i] != '\0'; i++) {
        if (zb_is_path_sep(work[i])) {
            char saved = work[i];
            work[i] = '\0';
            /* A bare drive letter ("C:") is not a directory to create. */
            if (!(i == 2 && work[1] == ':')) {
                if (mkdir_one(work) != 0) {
                    rc = -1;
                    work[i] = saved;
                    goto done;
                }
            }
            work[i] = saved;
        }
    }
    rc = mkdir_one(work);

done:
    zb_free(work);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Files                                                                */
/* ------------------------------------------------------------------ */

int zb_stat(const char *path, zb_stat_info *out)
{
#ifdef _WIN32
    struct __stat64 st;
    wchar_t *w = widen(path);
    int rc;
    if (w == NULL) {
        return -1;
    }
    rc = _wstat64(w, &st);
    zb_free(w);
    if (rc != 0) {
        return -1;
    }
    out->size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
    out->mtime = (int64_t)st.st_mtime;
    out->is_dir = (st.st_mode & _S_IFDIR) != 0;
    out->is_regular = (st.st_mode & _S_IFREG) != 0;
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    out->size = st.st_size < 0 ? 0 : (uint64_t)st.st_size;
    out->mtime = (int64_t)st.st_mtime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    return 0;
#endif
}

int zb_path_exists(const char *path)
{
    zb_stat_info info;
    return zb_stat(path, &info) == 0;
}

FILE *zb_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wpath;
    wchar_t wmode[8];
    FILE *fp = NULL;
    size_t i;
    size_t n;

    wpath = widen(path);
    if (wpath == NULL) {
        return NULL;
    }
    n = strlen(mode);
    if (n >= sizeof(wmode) / sizeof(wmode[0])) {
        zb_free(wpath);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        wmode[i] = (wchar_t)(unsigned char)mode[i];
    }
    wmode[n] = L'\0';
    fp = _wfopen(wpath, wmode);
    zb_free(wpath);
    return fp;
#else
    return fopen(path, mode);
#endif
}

FILE *zb_fopen_private_new(const char *path)
{
#ifdef _WIN32
    /* O_EXCL semantics via CreateFileW, then hand the handle to the CRT. The
     * file inherits the user's profile ACL; see the note in `shelf export`. */
    wchar_t *w = widen(path);
    HANDLE h;
    int fd;
    FILE *fp;

    if (w == NULL) {
        return NULL;
    }
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    zb_free(w);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    fd = _open_osfhandle((intptr_t)h, _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        return NULL;
    }
    fp = _fdopen(fd, "wb");
    if (fp == NULL) {
        _close(fd);
        return NULL;
    }
    return fp;
#else
    /* O_EXCL plus an explicit 0600 mode: correct from the instant the file
     * exists, with no create-then-chmod race and no dependence on umask. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *fp;
    if (fd < 0) {
        return NULL;
    }
    fp = fdopen(fd, "wb");
    if (fp == NULL) {
        close(fd);
        return NULL;
    }
    return fp;
#endif
}

int zb_fseek64(FILE *fp, int64_t offset, int whence)
{
#ifdef _WIN32
    return _fseeki64(fp, offset, whence) == 0 ? 0 : -1;
#else
    return fseeko(fp, (off_t)offset, whence) == 0 ? 0 : -1;
#endif
}

int64_t zb_ftell64(FILE *fp)
{
#ifdef _WIN32
    return (int64_t)_ftelli64(fp);
#else
    return (int64_t)ftello(fp);
#endif
}

int zb_fsync(FILE *fp)
{
    if (fflush(fp) != 0) {
        return -1;
    }
#ifdef _WIN32
    {
        HANDLE h = (HANDLE)_get_osfhandle(_fileno(fp));
        if (h == INVALID_HANDLE_VALUE) {
            return -1;
        }
        return FlushFileBuffers(h) ? 0 : -1;
    }
#else
    return fsync(fileno(fp)) == 0 ? 0 : -1;
#endif
}

int zb_remove(const char *path)
{
#ifdef _WIN32
    wchar_t *w = widen(path);
    int rc;
    if (w == NULL) {
        return -1;
    }
    rc = DeleteFileW(w) ? 0 : -1;
    zb_free(w);
    return rc;
#else
    return remove(path) == 0 ? 0 : -1;
#endif
}

int zb_rename_replace(const char *from, const char *to)
{
#ifdef _WIN32
    /* Plain rename() fails on Windows when the destination exists, which is
     * exactly the case every atomic write hits. */
    wchar_t *wf = widen(from);
    wchar_t *wt = widen(to);
    int rc = -1;
    if (wf != NULL && wt != NULL) {
        rc = MoveFileExW(wf, wt,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                 ? 0
                 : -1;
    }
    zb_free(wf);
    zb_free(wt);
    return rc;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

int zb_temp_file(const char *prefix, char **path_out, FILE **fp_out)
{
    char *dir;
    unsigned attempt;

    *path_out = NULL;
    *fp_out = NULL;

#ifdef _WIN32
    {
        wchar_t wdir[MAX_PATH + 1];
        DWORD n = GetTempPathW(MAX_PATH, wdir);
        if (n == 0 || n > MAX_PATH) {
            return -1;
        }
        dir = narrow(wdir);
    }
#else
    dir = zb_getenv_dup("TMPDIR");
    if (dir == NULL) {
        dir = zb_strdup("/tmp");
    }
#endif
    if (dir == NULL) {
        return -1;
    }

    for (attempt = 0; attempt < 64; attempt++) {
        char name[128];
        char *path;
        FILE *fp;
        uint64_t salt = zb_now_ms() ^ ((uint64_t)attempt << 40);

#ifdef _WIN32
        salt ^= (uint64_t)GetCurrentProcessId() << 16;
#else
        salt ^= (uint64_t)getpid() << 16;
#endif
        if (snprintf(name, sizeof(name), "%s-%llx.tmp", prefix,
                     (unsigned long long)salt) < 0) {
            break;
        }
        path = zb_path_join(dir, name);
        if (path == NULL) {
            break;
        }
        fp = zb_fopen_private_new(path);
        if (fp != NULL) {
            /* Reopen read/write: the private-create path is write-only. */
            fclose(fp);
            fp = zb_fopen(path, "w+b");
            if (fp == NULL) {
                zb_remove(path);
                zb_free(path);
                continue;
            }
            zb_free(dir);
            *path_out = path;
            *fp_out = fp;
            return 0;
        }
        zb_free(path);
    }

    zb_free(dir);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Time                                                                 */
/* ------------------------------------------------------------------ */

uint64_t zb_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (uint64_t)time(NULL) * 1000u;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000L);
#endif
}

int64_t zb_now_unix(void)
{
    return (int64_t)time(NULL);
}

/* Days from 1970-01-01 to the given civil date. Howard Hinnant's algorithm —
 * exact, branch-light, and independent of the platform's broken-down time
 * support, which is why we do not need timegm(). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    /* (unsigned)-3 rather than -3u: the two compile to the identical
     * wraparound value (UINT_MAX - 2), but get there differently. -3u
     * negates an operand that is already unsigned, which is exactly what
     * MSVC's C4146 warns about; (unsigned)-3 negates a signed literal and
     * casts the result, which does not. Because the cast is explicit rather
     * than an implicit conversion, it also does not trip GCC/Clang's
     * -Wsign-conversion the way a bare "-3" added to unsigned `m` would. */
    doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y_out, unsigned *m_out,
                            unsigned *d_out)
{
    int64_t era;
    unsigned doe;
    unsigned yoe;
    int64_t y;
    unsigned doy;
    unsigned mp;
    unsigned d;
    unsigned m;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    y = (int64_t)yoe + era * 400;
    doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    mp = (5u * doy + 2u) / 153u;
    d = doy - (153u * mp + 2u) / 5u + 1u;
    m = mp + (mp < 10u ? 3u : (unsigned)-9);
    y += (m <= 2);

    *y_out = (int)y;
    *m_out = m;
    *d_out = d;
}

int64_t zb_timegm_utc(int year, int mon, int mday, int hour, int min, int sec)
{
    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)mday);
    return days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

void zb_gmtime_utc(int64_t t, int *year, int *mon, int *mday, int *hour,
                   int *min, int *sec)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    unsigned m;
    unsigned d;

    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    civil_from_days(days, year, &m, &d);
    *mon = (int)m;
    *mday = (int)d;
    *hour = (int)(rem / 3600);
    *min = (int)((rem % 3600) / 60);
    *sec = (int)(rem % 60);
}

void zb_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}
