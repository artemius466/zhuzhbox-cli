#include "util/str.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "util/buf.h"

int zb_snprintf(char *dst, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (size == 0) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(dst, size, fmt, ap);
    va_end(ap);
    if (n < 0) {
        dst[0] = '\0';
        return -1;
    }
    if ((size_t)n >= size) {
        return -1;
    }
    return 0;
}

static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

int zb_streq_ci(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    while (*a != '\0' && *b != '\0') {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int zb_str_starts_with(const char *s, const char *prefix)
{
    size_t n;
    if (s == NULL || prefix == NULL) {
        return 0;
    }
    n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

int zb_str_ends_with_ci(const char *s, const char *suffix)
{
    size_t ls;
    size_t lx;
    size_t i;

    if (s == NULL || suffix == NULL) {
        return 0;
    }
    ls = strlen(s);
    lx = strlen(suffix);
    if (lx > ls) {
        return 0;
    }
    for (i = 0; i < lx; i++) {
        if (ascii_lower(s[ls - lx + i]) != ascii_lower(suffix[i])) {
            return 0;
        }
    }
    return 1;
}

/* Decode one UTF-8 sequence. Returns the number of bytes consumed (>= 1) and
 * stores the code point; invalid bytes decode as U+FFFD consuming one byte. */
static size_t utf8_decode(const unsigned char *s, unsigned long *cp_out)
{
    unsigned char c = s[0];
    unsigned long cp;
    size_t need;
    size_t i;

    if (c < 0x80u) {
        *cp_out = c;
        return 1;
    }
    if ((c & 0xE0u) == 0xC0u) {
        cp = c & 0x1Fu;
        need = 1;
    } else if ((c & 0xF0u) == 0xE0u) {
        cp = c & 0x0Fu;
        need = 2;
    } else if ((c & 0xF8u) == 0xF0u) {
        cp = c & 0x07u;
        need = 3;
    } else {
        *cp_out = 0xFFFDu;
        return 1;
    }
    for (i = 1; i <= need; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            *cp_out = 0xFFFDu;
            return 1;
        }
        cp = (cp << 6) | (unsigned long)(s[i] & 0x3Fu);
    }
    *cp_out = cp;
    return need + 1;
}

size_t zb_utf8_len(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    unsigned long cp;

    if (s == NULL) {
        return 0;
    }
    while (*p != '\0') {
        p += utf8_decode(p, &cp);
        n++;
    }
    return n;
}

size_t zb_utf8_offset(const char *s, size_t n)
{
    const unsigned char *start = (const unsigned char *)s;
    const unsigned char *p = start;
    unsigned long cp;
    size_t i = 0;

    if (s == NULL) {
        return 0;
    }
    while (*p != '\0' && i < n) {
        p += utf8_decode(p, &cp);
        i++;
    }
    return (size_t)(p - start);
}

/* Minimal wcwidth: zero for combining marks and zero-width controls, two for
 * the ranges that are double-width in every terminal that matters. */
static int cp_width(unsigned long cp)
{
    if (cp == 0) {
        return 0;
    }
    if (cp < 32u || (cp >= 0x7Fu && cp < 0xA0u)) {
        return 0;
    }
    /* Combining marks and other zero-width code points. */
    if ((cp >= 0x0300u && cp <= 0x036Fu) || (cp >= 0x0483u && cp <= 0x0489u) ||
        (cp >= 0x0591u && cp <= 0x05BDu) || (cp >= 0x0610u && cp <= 0x061Au) ||
        (cp >= 0x064Bu && cp <= 0x065Fu) || (cp >= 0x0E31u && cp <= 0x0E3Au) ||
        (cp >= 0x1AB0u && cp <= 0x1AFFu) || (cp >= 0x1DC0u && cp <= 0x1DFFu) ||
        (cp >= 0x20D0u && cp <= 0x20F0u) || (cp >= 0xFE00u && cp <= 0xFE0Fu) ||
        (cp >= 0xFE20u && cp <= 0xFE2Fu) || cp == 0x200Bu || cp == 0x200Cu ||
        cp == 0x200Du || cp == 0xFEFFu) {
        return 0;
    }
    /* Wide ranges: CJK, Hangul, Kana, fullwidth forms, and the emoji planes. */
    if ((cp >= 0x1100u && cp <= 0x115Fu) || (cp >= 0x2E80u && cp <= 0x303Eu) ||
        (cp >= 0x3041u && cp <= 0x33FFu) || (cp >= 0x3400u && cp <= 0x4DBFu) ||
        (cp >= 0x4E00u && cp <= 0x9FFFu) || (cp >= 0xA000u && cp <= 0xA4CFu) ||
        (cp >= 0xAC00u && cp <= 0xD7A3u) || (cp >= 0xF900u && cp <= 0xFAFFu) ||
        (cp >= 0xFE30u && cp <= 0xFE6Fu) || (cp >= 0xFF00u && cp <= 0xFF60u) ||
        (cp >= 0xFFE0u && cp <= 0xFFE6u) || (cp >= 0x1F300u && cp <= 0x1F64Fu) ||
        (cp >= 0x1F680u && cp <= 0x1F6FFu) || (cp >= 0x1F900u && cp <= 0x1F9FFu) ||
        (cp >= 0x20000u && cp <= 0x3FFFDu)) {
        return 2;
    }
    return 1;
}

size_t zb_display_width(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t w = 0;
    unsigned long cp;

    if (s == NULL) {
        return 0;
    }
    while (*p != '\0') {
        p += utf8_decode(p, &cp);
        w += (size_t)cp_width(cp);
    }
    return w;
}

void zb_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    if (p == NULL) {
        return;
    }
    while (n-- > 0) {
        *v++ = 0;
    }
}

void zb_free_secret(char *s)
{
    if (s == NULL) {
        return;
    }
    zb_secure_zero(s, strlen(s));
    zb_free(s);
}

/* Windows reserved device names. Rejected on every platform so a shelf written
 * on Linux stays portable and so behavior does not vary by host. */
static int is_reserved_device_name(const char *name)
{
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9", NULL};
    size_t base_len = 0;
    const char *dot;
    size_t i;

    dot = strchr(name, '.');
    base_len = dot != NULL ? (size_t)(dot - name) : strlen(name);
    for (i = 0; reserved[i] != NULL; i++) {
        size_t rl = strlen(reserved[i]);
        size_t k;
        if (rl != base_len) {
            continue;
        }
        for (k = 0; k < rl; k++) {
            if (ascii_lower(name[k]) != ascii_lower(reserved[i][k])) {
                break;
            }
        }
        if (k == rl) {
            return 1;
        }
    }
    return 0;
}

#define ZB_SANITIZED_MAX 200

static char *sanitize_component(const char *name, const char *fallback)
{
    const char *base;
    const char *p;
    zb_buf out;
    size_t cut;
    char *result;

    if (name == NULL) {
        return zb_strdup(fallback);
    }

    /* Keep only what follows the last path separator of either flavor, so
     * "../../etc/passwd" and "C:\\Windows\\x" both collapse to a leaf name. */
    base = name;
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            base = p + 1;
        }
    }

    zb_buf_init(&out);
    for (p = base; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        /* Drop C0/C1 controls and the characters Windows forbids outright. */
        if (c < 0x20u || c == 0x7Fu) {
            continue;
        }
        if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' ||
            c == '*') {
            continue;
        }
        if (zb_buf_append_char(&out, (char)c) != 0) {
            zb_buf_free(&out);
            return NULL;
        }
    }

    /* Truncate on a code point boundary so a long CJK name stays valid UTF-8. */
    if (out.len > ZB_SANITIZED_MAX) {
        cut = ZB_SANITIZED_MAX;
        while (cut > 0 && ((unsigned char)out.ptr[cut] & 0xC0u) == 0x80u) {
            cut--;
        }
        out.len = cut;
        out.ptr[out.len] = '\0';
    }

    /* Trailing dots and spaces are silently stripped by Windows, which would
     * make the name we report differ from the name on disk. */
    while (out.len > 0 &&
           (out.ptr[out.len - 1] == '.' || out.ptr[out.len - 1] == ' ')) {
        out.len--;
        out.ptr[out.len] = '\0';
    }
    /* Leading dots would hide the file; a bare "." or ".." must never survive. */
    {
        size_t lead = 0;
        while (lead < out.len && out.ptr[lead] == '.') {
            lead++;
        }
        if (lead == out.len) {
            out.len = 0;
            if (out.ptr != NULL) {
                out.ptr[0] = '\0';
            }
        }
    }

    if (out.len == 0 || is_reserved_device_name(zb_buf_str(&out))) {
        zb_buf_free(&out);
        return zb_strdup(fallback);
    }

    result = zb_buf_detach(&out);
    zb_buf_free(&out);
    return result;
}

char *zb_sanitize_filename(const char *name)
{
    return sanitize_component(name, "download");
}

char *zb_sanitize_dirname(const char *name)
{
    return sanitize_component(name, "collection");
}

char *zb_url_escape_path(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    zb_buf out;
    const unsigned char *p;
    char *result;

    if (s == NULL) {
        return NULL;
    }
    zb_buf_init(&out);
    for (p = (const unsigned char *)s; *p != '\0'; p++) {
        unsigned char c = *p;
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                         c == '.' || c == '~';
        int ok;
        if (unreserved) {
            ok = zb_buf_append_char(&out, (char)c) == 0;
        } else {
            char enc[3];
            enc[0] = '%';
            enc[1] = hex[(c >> 4) & 0x0Fu];
            enc[2] = hex[c & 0x0Fu];
            ok = zb_buf_append(&out, enc, sizeof(enc)) == 0;
        }
        if (!ok) {
            zb_buf_free(&out);
            return NULL;
        }
    }
    result = zb_buf_detach(&out);
    zb_buf_free(&out);
    return result;
}

char *zb_str_trim(char *s)
{
    char *end;
    if (s == NULL) {
        return NULL;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                       end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}
