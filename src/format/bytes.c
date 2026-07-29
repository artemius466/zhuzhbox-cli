#include "format/bytes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "util/str.h"

static const char *const k_units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};

char *zb_format_bytes(uint64_t bytes, char *dst, size_t size)
{
    double value = (double)bytes;
    size_t unit = 0;

    while (value >= 1024.0 && unit + 1 < sizeof(k_units) / sizeof(k_units[0])) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0) {
        (void)zb_snprintf(dst, size, "%llu B", (unsigned long long)bytes);
    } else if (value < 10.0) {
        (void)zb_snprintf(dst, size, "%.2f %s", value, k_units[unit]);
    } else if (value < 100.0) {
        (void)zb_snprintf(dst, size, "%.1f %s", value, k_units[unit]);
    } else {
        (void)zb_snprintf(dst, size, "%.0f %s", value, k_units[unit]);
    }
    return dst;
}

char *zb_format_rate(double bytes_per_second, char *dst, size_t size)
{
    char buf[ZB_BYTES_BUF];

    if (!isfinite(bytes_per_second) || bytes_per_second <= 0.0) {
        (void)zb_snprintf(dst, size, "--");
        return dst;
    }
    if (bytes_per_second > 1e18) {
        bytes_per_second = 1e18;
    }
    zb_format_bytes((uint64_t)bytes_per_second, buf, sizeof(buf));
    (void)zb_snprintf(dst, size, "%s/s", buf);
    return dst;
}

char *zb_format_duration(uint64_t seconds, char *dst, size_t size)
{
    uint64_t days;
    uint64_t hours;
    uint64_t mins;
    uint64_t secs;

    if (seconds == 0) {
        (void)zb_snprintf(dst, size, "0s");
        return dst;
    }
    days = seconds / 86400;
    hours = (seconds % 86400) / 3600;
    mins = (seconds % 3600) / 60;
    secs = seconds % 60;

    if (days > 0) {
        (void)zb_snprintf(dst, size, "%llud %lluh", (unsigned long long)days,
                          (unsigned long long)hours);
    } else if (hours > 0) {
        (void)zb_snprintf(dst, size, "%lluh %llum", (unsigned long long)hours,
                          (unsigned long long)mins);
    } else if (mins > 0) {
        (void)zb_snprintf(dst, size, "%llum %llus", (unsigned long long)mins,
                          (unsigned long long)secs);
    } else {
        (void)zb_snprintf(dst, size, "%llus", (unsigned long long)secs);
    }
    return dst;
}

int zb_parse_size(const char *s, uint64_t *out)
{
    double value = 0.0;
    int digits = 0;
    uint64_t multiplier = 1;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    while (*s >= '0' && *s <= '9') {
        value = value * 10.0 + (double)(*s - '0');
        s++;
        digits++;
    }
    if (*s == '.') {
        double scale = 0.1;
        s++;
        while (*s >= '0' && *s <= '9') {
            value += (double)(*s - '0') * scale;
            scale /= 10.0;
            s++;
            digits++;
        }
    }
    if (digits == 0) {
        return -1;
    }
    while (*s == ' ') {
        s++;
    }
    if (*s != '\0') {
        if (zb_streq_ci(s, "b")) {
            multiplier = 1;
        } else if (zb_streq_ci(s, "k") || zb_streq_ci(s, "kb") ||
                   zb_streq_ci(s, "kib")) {
            multiplier = UINT64_C(1024);
        } else if (zb_streq_ci(s, "m") || zb_streq_ci(s, "mb") ||
                   zb_streq_ci(s, "mib")) {
            multiplier = UINT64_C(1024) * 1024;
        } else if (zb_streq_ci(s, "g") || zb_streq_ci(s, "gb") ||
                   zb_streq_ci(s, "gib")) {
            multiplier = UINT64_C(1024) * 1024 * 1024;
        } else if (zb_streq_ci(s, "t") || zb_streq_ci(s, "tb") ||
                   zb_streq_ci(s, "tib")) {
            multiplier = UINT64_C(1024) * 1024 * 1024 * 1024;
        } else {
            return -1;
        }
    }

    value *= (double)multiplier;
    if (!isfinite(value) || value < 0.0 || value > 9007199254740992.0) {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}
