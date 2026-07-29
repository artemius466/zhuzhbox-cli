/* unit_tests.c — the pure functions: token extraction, UTF-8 measurement,
 * filename sanitizing, ISO-8601 parsing, size formatting and the JSON number
 * validation. No network, no filesystem, no fixtures. */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "format/bytes.h"
#include "format/time.h"
#include "util/buf.h"
#include "util/error.h"
#include "util/json.h"
#include "util/mime.h"
#include "util/platform.h"
#include "util/str.h"
#include "util/token.h"
#include "zb_limits.h"

static int g_pass;
static int g_fail;

static void check(int condition, const char *what)
{
    if (condition) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static void check_str(const char *actual, const char *expected, const char *what)
{
    if (actual != NULL && expected != NULL && strcmp(actual, expected) == 0) {
        g_pass++;
        return;
    }
    if (actual == NULL && expected == NULL) {
        g_pass++;
        return;
    }
    g_fail++;
    printf("  FAIL %s: expected [%s], got [%s]\n", what,
           expected != NULL ? expected : "(null)",
           actual != NULL ? actual : "(null)");
}

/* Runs zb_extract_token and compares, freeing the result. */
static void check_token(const char *input, const char *expected)
{
    char *got = zb_extract_token(input);
    check_str(got, expected, input != NULL ? input : "(null)");
    zb_free(got);
}

static void test_tokens(void)
{
    puts("== token extraction");

    check_token("abc123XYZ0", "abc123XYZ0");
    check_token("https://zhuzhbox.fun/d/abc123XYZ0", "abc123XYZ0");
    check_token("http://dl.zhuzhbox.fun/d/abc123XYZ0?inline=1", "abc123XYZ0");
    check_token("https://zhuzhbox.fun/d/abc123XYZ0#frag", "abc123XYZ0");
    check_token("/d/abc123XYZ0", "abc123XYZ0");
    check_token("d/abc123XYZ0/", "abc123XYZ0");
    check_token("  https://zhuzhbox.fun/d/abc123XYZ0  ", "abc123XYZ0");

    /* Too short, too long, wrong characters, or nothing token-shaped at all. */
    check_token("abc12", NULL);
    check_token("abc-123-xyz", NULL);
    check_token("", NULL);
    check_token(NULL, NULL);
    check_token("https://zhuzhbox.fun", NULL);
    check_token("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL); /* 33 chars */

    check(zb_is_token("abcdef"), "six characters is a token");
    check(!zb_is_token("abcde"), "five characters is not");
    check(!zb_is_token("abcdef!"), "punctuation is not a token");
    check(!zb_is_token(NULL), "NULL is not a token");
}

static void check_sanitized(const char *input, const char *expected)
{
    char *got = zb_sanitize_filename(input);
    check_str(got, expected, input != NULL ? input : "(null)");
    zb_free(got);
}

static void test_sanitize(void)
{
    puts("== filename sanitizing");

    check_sanitized("report.pdf", "report.pdf");
    check_sanitized("../../etc/passwd", "passwd");
    check_sanitized("/etc/shadow", "shadow");
    check_sanitized("C:\\Windows\\System32\\evil.dll", "evil.dll");
    check_sanitized("..", "download");
    check_sanitized(".", "download");
    check_sanitized("...", "download");
    check_sanitized("", "download");
    check_sanitized(NULL, "download");
    /* Windows strips trailing dots as well as spaces, so we do too. */
    check_sanitized("trailing.  ", "trailing");
    check_sanitized("with\nnewline.txt", "withnewline.txt");
    check_sanitized("with\ttab.txt", "withtab.txt");
    check_sanitized("qu\"ote<>|?*.txt", "quote.txt");
    /* Windows device names are refused on every platform, so a shelf written
     * on Linux stays portable. */
    check_sanitized("CON", "download");
    check_sanitized("nul.txt", "download");
    check_sanitized("COM1.log", "download");
    check_sanitized("console.txt", "console.txt"); /* not a device name */
    /* Non-ASCII survives intact. */
    check_sanitized("файл.txt", "файл.txt");
    check_sanitized("日本語.pdf", "日本語.pdf");

    {
        /* Truncation must land on a code point boundary, never mid-sequence. */
        char long_name[600];
        char *got;
        size_t i;
        for (i = 0; i + 2 < sizeof(long_name); i += 2) {
            long_name[i] = (char)0xD0;     /* Cyrillic lead byte */
            long_name[i + 1] = (char)0xB0; /* 'а' */
        }
        long_name[i] = '\0';
        got = zb_sanitize_filename(long_name);
        check(got != NULL, "long name sanitizes");
        if (got != NULL) {
            /* Every byte must still decode: an odd length would mean the
             * truncation split a two-byte sequence. */
            check(strlen(got) % 2 == 0, "truncation kept UTF-8 well formed");
            check(strlen(got) <= 200, "truncation respected the cap");
        }
        zb_free(got);
    }
}

static void test_utf8(void)
{
    puts("== UTF-8 measurement");

    check(zb_utf8_len("hello") == 5, "ASCII length");
    check(zb_utf8_len("файл") == 4, "Cyrillic counts code points, not bytes");
    check(strlen("файл") == 8, "...and that string really is 8 bytes");
    check(zb_utf8_len("日本語") == 3, "CJK length");
    check(zb_utf8_len("") == 0, "empty length");
    check(zb_utf8_len(NULL) == 0, "NULL length");

    check(zb_display_width("hello") == 5, "ASCII width");
    check(zb_display_width("файл") == 4, "Cyrillic is single width");
    check(zb_display_width("日本語") == 6, "CJK is double width");
    check(zb_display_width("🙂") == 2, "emoji is double width");

    check(zb_utf8_offset("файл", 2) == 4, "offset of the third code point");
    check(zb_utf8_offset("файл", 99) == 8, "offset past the end clamps");
}

static void test_time(void)
{
    int64_t epoch = 0;
    char buffer[ZB_TIME_BUF];

    puts("== time");

    check(zb_time_parse_iso8601("2026-07-29T10:11:00.000Z", &epoch) == 0,
          "parses the API's timestamp format");
    check(epoch == 1785319860, "parsed epoch is right");

    check(zb_time_parse_iso8601("2026-07-29T10:11:00Z", &epoch) == 0,
          "fractional seconds are optional");
    check(epoch == 1785319860, "epoch without fraction matches");

    check(zb_time_parse_iso8601("1970-01-01T00:00:00Z", &epoch) == 0 &&
              epoch == 0,
          "the epoch itself");
    check(zb_time_parse_iso8601("2000-02-29T12:00:00Z", &epoch) == 0 &&
              epoch == 951825600,
          "leap day in a leap century");

    /* Anything that is not the one shape the API emits is rejected outright
     * rather than guessed at. */
    check(zb_time_parse_iso8601("not a date", &epoch) != 0, "rejects prose");
    check(zb_time_parse_iso8601("2026-13-01T00:00:00Z", &epoch) != 0,
          "rejects month 13");
    check(zb_time_parse_iso8601("2026-07-29", &epoch) != 0,
          "rejects a bare date");
    check(zb_time_parse_iso8601("2026-07-29T10:11:00+05:00", &epoch) != 0,
          "rejects a non-UTC offset instead of mis-converting it");
    check(zb_time_parse_iso8601("2026-07-29T10:11:00Zjunk", &epoch) != 0,
          "rejects trailing junk");
    check(zb_time_parse_iso8601(NULL, &epoch) != 0, "rejects NULL");

    zb_time_format_iso8601(1785319860, buffer, sizeof(buffer));
    check_str(buffer, "2026-07-29T10:11:00Z", "round-trips through formatting");

    zb_time_relative(1000, 1000, buffer, sizeof(buffer));
    check_str(buffer, "just now", "relative: now");
    zb_time_relative(0, 3600, buffer, sizeof(buffer));
    check_str(buffer, "1 hour ago", "relative: an hour ago");
    zb_time_relative(3600, 0, buffer, sizeof(buffer));
    check_str(buffer, "in 1 hour", "relative: in an hour");
    zb_time_relative(86400 * 3, 0, buffer, sizeof(buffer));
    check_str(buffer, "in 3 days", "relative: in three days");

    check(zb_time_until(100, 50) == 50, "seconds remaining");
    check(zb_time_until(50, 100) == 0, "already past clamps to zero");

    /* Retention tiers are checked top-down: the biggest match wins. */
    check(zb_retention_days_for_size(0) == 30, "tiny file keeps 30 days");
    check(zb_retention_days_for_size(UINT64_C(4) * 1024 * 1024 * 1024) == 30,
          "4 GiB keeps 30 days");
    check(zb_retention_days_for_size(UINT64_C(5) * 1024 * 1024 * 1024) == 15,
          "5 GiB keeps 15 days");
    check(zb_retention_days_for_size(UINT64_C(15) * 1024 * 1024 * 1024) == 7,
          "15 GiB keeps 7 days");
    check(zb_retention_days_for_size(UINT64_C(20) * 1024 * 1024 * 1024) == 3,
          "20 GiB keeps 3 days");
    check(zb_retention_days_for_size(ZB_MAX_UPLOAD_BYTES) == 3,
          "the largest allowed file keeps 3 days");
}

static void test_bytes(void)
{
    char buffer[ZB_BYTES_BUF];
    uint64_t parsed = 0;

    puts("== byte formatting");

    check_str(zb_format_bytes(0, buffer, sizeof(buffer)), "0 B", "zero bytes");
    check_str(zb_format_bytes(999, buffer, sizeof(buffer)), "999 B", "raw bytes");
    check_str(zb_format_bytes(1024, buffer, sizeof(buffer)), "1.00 KiB", "1 KiB");
    check_str(zb_format_bytes(1536, buffer, sizeof(buffer)), "1.50 KiB",
              "1.5 KiB");
    check_str(zb_format_bytes(UINT64_C(25) * 1024 * 1024 * 1024, buffer,
                              sizeof(buffer)),
              "25.0 GiB", "the upload cap");

    check(zb_parse_size("100", &parsed) == 0 && parsed == 100, "plain number");
    check(zb_parse_size("20M", &parsed) == 0 && parsed == 20u * 1024 * 1024,
          "20M");
    check(zb_parse_size("1.5GiB", &parsed) == 0 &&
              parsed == (uint64_t)(1.5 * 1024 * 1024 * 1024),
          "1.5GiB");
    check(zb_parse_size("nonsense", &parsed) != 0, "rejects prose");
    check(zb_parse_size("10Q", &parsed) != 0, "rejects an unknown unit");
    check(zb_parse_size("", &parsed) != 0, "rejects empty");
}

static void test_json_numbers(void)
{
    zb_json *root;
    uint64_t value = 0;

    puts("== JSON number validation");

    /* cJSON stores every number as a double. Integers below 2^53 are exact,
     * which covers 25 GiB comfortably; anything outside that must be refused
     * rather than silently truncated by a cast. */
    {
        static const char text[] = "{\"a\":26843545600,\"b\":-1,\"c\":1.5,"
                                   "\"d\":1e300,\"e\":\"12\",\"f\":0}";
        root = zb_json_parse(text, sizeof(text) - 1);
    }
    check(root != NULL, "parses");
    check(zb_json_get_u64(root, "a", &value) && value == 26843545600ULL,
          "25 GiB is exact");
    check(!zb_json_get_u64(root, "b", &value), "negative is refused");
    check(!zb_json_get_u64(root, "c", &value), "fractional is refused");
    check(!zb_json_get_u64(root, "d", &value), "above 2^53 is refused");
    check(!zb_json_get_u64(root, "e", &value), "a string is not a number");
    check(zb_json_get_u64(root, "f", &value) && value == 0, "zero is fine");
    check(!zb_json_get_u64(root, "missing", &value), "a missing key is refused");
    zb_json_free(root);

    check(zb_json_parse("{not json", 9) == NULL, "malformed input yields NULL");
    check(zb_json_parse(NULL, 0) == NULL, "NULL input yields NULL");
}

static void test_buf(void)
{
    zb_buf buf;
    char *detached;

    puts("== buffers");

    zb_buf_init(&buf);
    check_str(zb_buf_str(&buf), "", "an empty buffer reads as an empty string");
    check(zb_buf_append_str(&buf, "hello") == 0, "append");
    check(zb_buf_append_str(&buf, " world") == 0, "append again");
    check_str(zb_buf_str(&buf), "hello world", "contents");
    check(buf.len == 11, "length");
    check(zb_buf_printf(&buf, " %d", 42) == 0, "printf append");
    check_str(zb_buf_str(&buf), "hello world 42", "contents after printf");
    zb_buf_clear(&buf);
    check_str(zb_buf_str(&buf), "", "clear empties without freeing");
    check(zb_buf_append_str(&buf, "again") == 0, "reuse after clear");
    detached = zb_buf_detach(&buf);
    check_str(detached, "again", "detach hands over the bytes");
    check(buf.ptr == NULL, "detach resets the buffer");
    zb_free(detached);
    zb_buf_free(&buf);

    /* Embedded NULs must survive: response bodies are not always text. */
    zb_buf_init(&buf);
    check(zb_buf_append(&buf, "a\0b", 3) == 0, "append with an embedded NUL");
    check(buf.len == 3, "length counts the NUL");
    zb_buf_free(&buf);

    {
        char *formatted = zb_asprintf("%s-%d", "x", 7);
        check_str(formatted, "x-7", "asprintf");
        zb_free(formatted);
    }
}

static void test_mime(void)
{
    puts("== mime guessing");

    check_str(zb_mime_from_path("a.png"), "image/png", "png");
    check_str(zb_mime_from_path("a.PNG"), "image/png", "extension is case-insensitive");
    check_str(zb_mime_from_path("/some/dir/a.mp4"), "video/mp4", "with a path");
    check_str(zb_mime_from_path("noext"), "application/octet-stream",
              "no extension falls back");
    check_str(zb_mime_from_path("a.unknownext"), "application/octet-stream",
              "unknown extension falls back");
    check_str(zb_mime_from_path("/dir.with.dots/file"),
              "application/octet-stream", "a dot in a parent dir is not an ext");
    check_str(zb_mime_from_path(NULL), "application/octet-stream", "NULL");
}

static void test_error(void)
{
    zb_error err;

    puts("== errors");

    zb_error_init(&err);
    check(err.status == ZB_OK, "starts clean");
    check(err.retry_after_seconds == -1, "no Retry-After by default");
    check_str(zb_error_message(&err), "no error", "message when clean");

    zb_error_setf(&err, ZB_ERR_HTTP, "Not found or expired.");
    check(err.status == ZB_ERR_HTTP, "status is set");
    check_str(zb_error_message(&err), "Not found or expired.",
              "the server's message is kept verbatim");

    zb_error_setf(&err, ZB_ERR_IO, "cannot read %s", "x.bin");
    check_str(zb_error_message(&err), "cannot read x.bin", "formatting works");

    {
        zb_error moved;
        zb_error_init(&moved);
        zb_error_move(&moved, &err);
        check_str(zb_error_message(&moved), "cannot read x.bin", "move carries the message");
        check(err.status == ZB_OK, "move leaves the source clean");
        zb_error_clear(&moved);
    }
    zb_error_clear(&err);
    check(err.message == NULL, "clear frees the message");

    check_str(zb_status_name(ZB_ERR_CANCELED), "canceled", "status name");
    check(zb_curl_message(0) == NULL, "CURLE_OK has no message");
    check(zb_curl_message(6) != NULL, "a real curl code has a message");
}

static void test_time_helpers(void)
{
    int year;
    int mon;
    int mday;
    int hour;
    int min;
    int sec;

    puts("== civil calendar");

    /* days_from_civil / civil_from_days round-trip, including across leap
     * years and century boundaries. */
    check(zb_timegm_utc(1970, 1, 1, 0, 0, 0) == 0, "epoch");
    check(zb_timegm_utc(2000, 3, 1, 0, 0, 0) == 951868800, "after a leap day");
    check(zb_timegm_utc(2100, 3, 1, 0, 0, 0) == 4107542400LL,
          "2100 is not a leap year");

    zb_gmtime_utc(1785319860, &year, &mon, &mday, &hour, &min, &sec);
    check(year == 2026 && mon == 7 && mday == 29 && hour == 10 && min == 11 &&
              sec == 0,
          "epoch converts back to the right civil date");

    zb_gmtime_utc(0, &year, &mon, &mday, &hour, &min, &sec);
    check(year == 1970 && mon == 1 && mday == 1, "epoch converts back");
}

int main(void)
{
    test_tokens();
    test_sanitize();
    test_utf8();
    test_time();
    test_time_helpers();
    test_bytes();
    test_json_numbers();
    test_buf();
    test_mime();
    test_error();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
