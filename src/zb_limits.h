/* limits.h — service limits. One place, so nothing hardcodes a duplicate. */
#ifndef ZB_LIMITS_H
#define ZB_LIMITS_H

#include <stdint.h>

/* Max per-file upload size: 25 GiB (server MAX_UPLOAD_BYTES). Written as a
 * 64-bit expression so a 32-bit build cannot truncate it. */
#define ZB_MAX_UPLOAD_BYTES (UINT64_C(25) * 1024 * 1024 * 1024)

/* Server MAX_FILES_PER_COLLECTION. */
#define ZB_MAX_FILES_PER_COLLECTION 200

/* Collection metadata limits, counted in UTF-8 code points, not bytes. */
#define ZB_MAX_TITLE_CHARS 200
#define ZB_MAX_DESCRIPTION_CHARS 4000

/* Abuse report note limit, also code points. */
#define ZB_MAX_REPORT_NOTE_CHARS 2000

/* Weekly quota: 30 GiB per uploader over a rolling 7-day window. Informational
 * only — the server is authoritative via /v1/quota. */
#define ZB_QUOTA_BYTES (UINT64_C(30) * 1024 * 1024 * 1024)
#define ZB_QUOTA_WINDOW_DAYS 7

/* Upload/collection inits allowed per IP per 15 minutes. */
#define ZB_UPLOAD_RATE_LIMIT_MAX 20
#define ZB_UPLOAD_RATE_LIMIT_WINDOW_MINUTES 15

/* An upload session the server has not seen activity on for this long is
 * discarded. Local sessions older than this are pruned on startup. */
#define ZB_UPLOAD_SESSION_TTL_MINUTES 180

/* Only a fallback for display and for the single-shot cutoff default: the real
 * chunk size always comes from the init response. */
#define ZB_DEFAULT_CHUNK_SIZE (UINT64_C(20) * 1024 * 1024)

/* Retention tiers, checked top-down — the biggest matching tier wins. */
#define ZB_RETENTION_TIERS                                                     \
    {                                                                          \
        {UINT64_C(20) * 1024 * 1024 * 1024, 3},                                \
        {UINT64_C(15) * 1024 * 1024 * 1024, 7},                                \
        {UINT64_C(5) * 1024 * 1024 * 1024, 15},                                \
        {0, 30},                                                               \
    }

/* Tokens accepted by POST /v1/uploads/exists per call. */
#define ZB_EXISTS_BATCH_MAX 500

/* Per-chunk retry policy (§3.1). */
#define ZB_CHUNK_MAX_RETRIES 5
#define ZB_CHUNK_BACKOFF_MS(attempt)                                           \
    ((unsigned)((1000u * ((unsigned)(attempt) + 1u)) > 5000u                   \
                    ? 5000u                                                    \
                    : (1000u * ((unsigned)(attempt) + 1u))))

/* A Retry-After longer than this is not worth sleeping through (§9). */
#define ZB_MAX_RETRY_AFTER_SECONDS 300

/* Public defaults (§1). */
#define ZB_DEFAULT_API_HOST "https://api.zhuzhbox.fun"
#define ZB_DEFAULT_DOWNLOAD_HOST "https://dl.zhuzhbox.fun"
#define ZB_DEFAULT_SITE_HOST "https://zhuzhbox.fun"

#define ZB_DEFAULT_CONCURRENCY 3
#define ZB_MAX_CONCURRENCY 16

#endif /* ZB_LIMITS_H */
