/* time.h — ISO-8601 parsing and relative timestamps.
 *
 * The API emits exactly one shape: "2026-07-29T10:11:00.000Z". There is no
 * portable strptime (MSVC lacks it) and no portable timegm, so this parses that
 * shape by hand and rejects anything else rather than guessing (§2.1). */
#ifndef ZB_FORMAT_TIME_H
#define ZB_FORMAT_TIME_H

#include <stddef.h>
#include <stdint.h>

#define ZB_TIME_BUF 40

/* Parse "YYYY-MM-DDTHH:MM:SS[.mmm]Z" (the fractional part and the trailing Z
 * are both optional; "+00:00" is accepted in place of Z). Stores epoch seconds.
 * Returns 0 on success, -1 if the input does not match. */
int zb_time_parse_iso8601(const char *s, int64_t *out_epoch);

/* Format epoch seconds as "2026-07-29T10:11:00Z". Returns `dst`. */
char *zb_time_format_iso8601(int64_t epoch, char *dst, size_t size);

/* Format epoch seconds as a local-looking absolute stamp, "2026-07-29 10:11
 * UTC". Returns `dst`. */
char *zb_time_format_stamp(int64_t epoch, char *dst, size_t size);

/* "3 minutes ago", "in 12 days", "just now". `now` is epoch seconds; pass the
 * result of zb_now_unix(). Returns `dst`. */
char *zb_time_relative(int64_t epoch, int64_t now, char *dst, size_t size);

/* Seconds until `epoch`, clamped at 0 if it has already passed. */
int64_t zb_time_until(int64_t epoch, int64_t now);

/* Retention window in days for a file of `size`, per the server's tiers. Used
 * only to explain expiry before an upload happens; the authoritative expiry
 * always comes back from the server. */
int zb_retention_days_for_size(uint64_t size);

#endif /* ZB_FORMAT_TIME_H */
