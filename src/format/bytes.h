/* bytes.h — human-readable sizes and rates. */
#ifndef ZB_FORMAT_BYTES_H
#define ZB_FORMAT_BYTES_H

#include <stddef.h>
#include <stdint.h>

/* Longest output is like "1023.9 GiB" — 16 bytes is comfortable. */
#define ZB_BYTES_BUF 24

/* Writes e.g. "4.7 GiB" into `dst`. Returns `dst`. */
char *zb_format_bytes(uint64_t bytes, char *dst, size_t size);

/* Writes e.g. "12.4 MiB/s", or "--" when the rate is not yet known. */
char *zb_format_rate(double bytes_per_second, char *dst, size_t size);

/* Writes e.g. "1m 12s", or "--" for an unknown duration. */
char *zb_format_duration(uint64_t seconds, char *dst, size_t size);

/* Parse "10", "20M", "1.5GiB" into bytes. 0 on success, -1 on bad input. */
int zb_parse_size(const char *s, uint64_t *out);

#endif /* ZB_FORMAT_BYTES_H */
