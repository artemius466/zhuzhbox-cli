/* progress.h — one redrawn line: percentage, transferred, speed, ETA.
 *
 * Speed comes from a rolling window over the last few seconds rather than a
 * cumulative average, so the number reacts to what the connection is doing now
 * instead of slowly forgetting a slow start (§5).
 *
 * Everything here writes to stderr, so stdout stays clean for --json and for
 * `get -o -`.
 */
#ifndef ZB_FORMAT_PROGRESS_H
#define ZB_FORMAT_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/* Samples covering roughly the last 3 seconds at ~10 Hz. */
#define ZB_PROGRESS_SAMPLES ((size_t)32)

typedef struct {
    uint64_t at_ms;
    uint64_t bytes;
} zb_progress_sample;

typedef struct {
    char *label; /* owned */
    uint64_t total;
    uint64_t current;
    uint64_t started_ms;
    uint64_t last_draw_ms;
    zb_progress_sample samples[ZB_PROGRESS_SAMPLES];
    size_t sample_count;
    size_t sample_head;
    int enabled;
    int drawn; /* a line is currently on screen and needs clearing */
} zb_progress;

/* `enabled` is zb_options.progress. When it is 0 every call is a no-op, so
 * callers never need to branch. */
void zb_progress_init(zb_progress *p, const char *label, uint64_t total,
                      int enabled);
void zb_progress_free(zb_progress *p);

/* Change the label mid-flight (e.g. per file in a bundle). */
void zb_progress_set_label(zb_progress *p, const char *label);
void zb_progress_set_total(zb_progress *p, uint64_t total);

/* Feed the current byte count. Redraws at most ~10x/second regardless of how
 * often libcurl calls back. */
void zb_progress_update(zb_progress *p, uint64_t current);

/* Erase the line without printing a summary — used before an error message. */
void zb_progress_clear(zb_progress *p);

/* Erase the line and print a one-line summary of what was transferred. */
void zb_progress_finish(zb_progress *p);

#endif /* ZB_FORMAT_PROGRESS_H */
