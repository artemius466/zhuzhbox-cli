#include "util/mime.h"

#include <string.h>

#include "util/str.h"

struct mime_entry {
    const char *ext; /* without the dot, lowercase */
    const char *mime;
};

static const struct mime_entry k_table[] = {
    /* text and code */
    {"txt", "text/plain"},
    {"md", "text/markdown"},
    {"csv", "text/csv"},
    {"log", "text/plain"},
    {"html", "text/html"},
    {"htm", "text/html"},
    {"css", "text/css"},
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"xml", "application/xml"},
    {"yaml", "application/yaml"},
    {"yml", "application/yaml"},
    {"toml", "application/toml"},
    {"c", "text/x-c"},
    {"h", "text/x-c"},
    {"py", "text/x-python"},
    {"sh", "application/x-sh"},
    /* images */
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"svg", "image/svg+xml"},
    {"bmp", "image/bmp"},
    {"ico", "image/vnd.microsoft.icon"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"heic", "image/heic"},
    /* audio and video */
    {"mp3", "audio/mpeg"},
    {"m4a", "audio/mp4"},
    {"aac", "audio/aac"},
    {"ogg", "audio/ogg"},
    {"opus", "audio/opus"},
    {"wav", "audio/wav"},
    {"flac", "audio/flac"},
    {"mp4", "video/mp4"},
    {"m4v", "video/mp4"},
    {"mkv", "video/x-matroska"},
    {"webm", "video/webm"},
    {"mov", "video/quicktime"},
    {"avi", "video/x-msvideo"},
    /* documents */
    {"pdf", "application/pdf"},
    {"epub", "application/epub+zip"},
    {"doc", "application/msword"},
    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml."
             "document"},
    {"xls", "application/vnd.ms-excel"},
    {"xlsx",
     "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {"ppt", "application/vnd.ms-powerpoint"},
    {"pptx", "application/vnd.openxmlformats-officedocument.presentationml."
             "presentation"},
    /* archives and binaries */
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
    {"bz2", "application/x-bzip2"},
    {"xz", "application/x-xz"},
    {"zst", "application/zstd"},
    {"tar", "application/x-tar"},
    {"7z", "application/x-7z-compressed"},
    {"rar", "application/vnd.rar"},
    {"iso", "application/x-iso9660-image"},
    {"deb", "application/vnd.debian.binary-package"},
    {"rpm", "application/x-rpm"},
    {"apk", "application/vnd.android.package-archive"},
    {"exe", "application/vnd.microsoft.portable-executable"},
    {"dmg", "application/x-apple-diskimage"},
    {"wasm", "application/wasm"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
};

#define ZB_MIME_DEFAULT "application/octet-stream"

const char *zb_mime_from_path(const char *path)
{
    const char *dot = NULL;
    const char *p;
    size_t i;

    if (path == NULL) {
        return ZB_MIME_DEFAULT;
    }
    for (p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            dot = NULL; /* a dot in a parent directory is not an extension */
        } else if (*p == '.') {
            dot = p + 1;
        }
    }
    if (dot == NULL || *dot == '\0') {
        return ZB_MIME_DEFAULT;
    }
    for (i = 0; i < sizeof(k_table) / sizeof(k_table[0]); i++) {
        if (zb_streq_ci(dot, k_table[i].ext)) {
            return k_table[i].mime;
        }
    }
    return ZB_MIME_DEFAULT;
}

const char *zb_ext_from_mime(const char *mime)
{
    static const struct {
        const char *mime;
        const char *ext;
    } k_reverse[] = {
        {"text/plain", ".txt"},        {"application/json", ".json"},
        {"image/png", ".png"},         {"image/jpeg", ".jpg"},
        {"image/gif", ".gif"},         {"image/webp", ".webp"},
        {"application/pdf", ".pdf"},   {"video/mp4", ".mp4"},
        {"audio/mpeg", ".mp3"},        {"application/zip", ".zip"},
        {"application/gzip", ".gz"},   {"text/html", ".html"},
    };
    size_t i;
    if (mime == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_reverse) / sizeof(k_reverse[0]); i++) {
        if (zb_streq_ci(mime, k_reverse[i].mime)) {
            return k_reverse[i].ext;
        }
    }
    return NULL;
}
