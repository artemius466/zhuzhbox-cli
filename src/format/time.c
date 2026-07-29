#include "format/time.h"

#include <stdio.h>
#include <string.h>

#include "zb_limits.h"
#include "util/platform.h"
#include "util/str.h"

static int digits2(const char *s, int *out)
{
    if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
        return -1;
    }
    *out = (s[0] - '0') * 10 + (s[1] - '0');
    return 0;
}

static int digits4(const char *s, int *out)
{
    int i;
    int v = 0;
    for (i = 0; i < 4; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

int zb_time_parse_iso8601(const char *s, int64_t *out_epoch)
{
    int year;
    int mon;
    int mday;
    int hour;
    int min;
    int sec;
    size_t len;

    if (s == NULL) {
        return -1;
    }
    len = strlen(s);
    /* The shortest accepted form is "YYYY-MM-DDTHH:MM:SS". */
    if (len < 19) {
        return -1;
    }
    if (digits4(s, &year) != 0 || s[4] != '-' || digits2(s + 5, &mon) != 0 ||
        s[7] != '-' || digits2(s + 8, &mday) != 0 ||
        (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        digits2(s + 11, &hour) != 0 || s[13] != ':' ||
        digits2(s + 14, &min) != 0 || s[16] != ':' ||
        digits2(s + 17, &sec) != 0) {
        return -1;
    }
    if (mon < 1 || mon > 12 || mday < 1 || mday > 31 || hour > 23 || min > 59 ||
        sec > 60) {
        return -1;
    }

    /* Optional fractional seconds, then an optional UTC designator. We only
     * ever see UTC from this API, so a non-UTC offset is rejected rather than
     * silently mis-converted. */
    {
        const char *p = s + 19;
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }
        if (*p == 'Z' || *p == 'z') {
            p++;
        } else if (strcmp(p, "+00:00") == 0 || strcmp(p, "+0000") == 0) {
            p += strlen(p);
        }
        if (*p != '\0') {
            return -1;
        }
    }

    *out_epoch = zb_timegm_utc(year, mon, mday, hour, min, sec);
    return 0;
}

char *zb_time_format_iso8601(int64_t epoch, char *dst, size_t size)
{
    int year;
    int mon;
    int mday;
    int hour;
    int min;
    int sec;

    zb_gmtime_utc(epoch, &year, &mon, &mday, &hour, &min, &sec);
    (void)zb_snprintf(dst, size, "%04d-%02d-%02dT%02d:%02d:%02dZ", year, mon,
                      mday, hour, min, sec);
    return dst;
}

char *zb_time_format_stamp(int64_t epoch, char *dst, size_t size)
{
    int year;
    int mon;
    int mday;
    int hour;
    int min;
    int sec;

    zb_gmtime_utc(epoch, &year, &mon, &mday, &hour, &min, &sec);
    (void)zb_snprintf(dst, size, "%04d-%02d-%02d %02d:%02d UTC", year, mon, mday,
                      hour, min);
    return dst;
}

int64_t zb_time_until(int64_t epoch, int64_t now)
{
    int64_t d = epoch - now;
    return d > 0 ? d : 0;
}

static void relative_magnitude(int64_t seconds, char *unit_out, size_t unit_size,
                               long long *value_out)
{
    long long v;
    const char *unit;

    if (seconds < 45) {
        v = seconds;
        unit = "second";
    } else if (seconds < 3600) {
        v = (seconds + 30) / 60;
        unit = "minute";
    } else if (seconds < 86400) {
        v = (seconds + 1800) / 3600;
        unit = "hour";
    } else if (seconds < 86400 * 30) {
        v = (seconds + 43200) / 86400;
        unit = "day";
    } else if (seconds < 86400 * 365) {
        v = (seconds + 1296000) / 2592000;
        unit = "month";
    } else {
        v = (seconds + 15768000) / 31536000;
        unit = "year";
    }
    if (v < 1) {
        v = 1;
    }
    *value_out = v;
    (void)zb_snprintf(unit_out, unit_size, "%s%s", unit, v == 1 ? "" : "s");
}

char *zb_time_relative(int64_t epoch, int64_t now, char *dst, size_t size)
{
    int64_t delta = epoch - now;
    char unit[16];
    long long value;

    if (delta <= 0) {
        int64_t ago = -delta;
        if (ago < 10) {
            (void)zb_snprintf(dst, size, "just now");
            return dst;
        }
        relative_magnitude(ago, unit, sizeof(unit), &value);
        (void)zb_snprintf(dst, size, "%lld %s ago", value, unit);
        return dst;
    }

    relative_magnitude(delta, unit, sizeof(unit), &value);
    (void)zb_snprintf(dst, size, "in %lld %s", value, unit);
    return dst;
}

int zb_retention_days_for_size(uint64_t size)
{
    struct tier {
        uint64_t min_bytes;
        int days;
    };
    static const struct tier k_tiers[] = ZB_RETENTION_TIERS;
    size_t i;

    /* Checked top-down: the biggest matching tier wins. */
    for (i = 0; i < sizeof(k_tiers) / sizeof(k_tiers[0]); i++) {
        if (size >= k_tiers[i].min_bytes) {
            return k_tiers[i].days;
        }
    }
    return 30;
}
