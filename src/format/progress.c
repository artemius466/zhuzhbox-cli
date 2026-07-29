#include "format/progress.h"

#include <stdio.h>
#include <string.h>

#include "format/bytes.h"
#include "util/buf.h"
#include "util/platform.h"
#include "util/str.h"

#define ZB_PROGRESS_MIN_REDRAW_MS 100

void zb_progress_init(zb_progress *p, const char *label, uint64_t total,
                      int enabled)
{
    memset(p, 0, sizeof(*p));
    p->enabled = enabled;
    p->total = total;
    p->started_ms = zb_now_ms();
    if (enabled && label != NULL) {
        p->label = zb_strdup(label);
    }
}

void zb_progress_free(zb_progress *p)
{
    if (p == NULL) {
        return;
    }
    zb_free(p->label);
    memset(p, 0, sizeof(*p));
}

void zb_progress_set_label(zb_progress *p, const char *label)
{
    char *copy;
    if (!p->enabled) {
        return;
    }
    copy = zb_strdup(label != NULL ? label : "");
    if (copy == NULL) {
        return; /* a missing label is not worth failing a transfer over */
    }
    zb_free(p->label);
    p->label = copy;
}

void zb_progress_set_total(zb_progress *p, uint64_t total)
{
    p->total = total;
}

/* Bytes per second over the oldest sample still inside the window. */
static double rolling_rate(const zb_progress *p)
{
    size_t oldest;
    uint64_t elapsed_ms;
    uint64_t delta;

    if (p->sample_count < 2) {
        return 0.0;
    }
    oldest = (p->sample_head + ZB_PROGRESS_SAMPLES - p->sample_count) %
             ZB_PROGRESS_SAMPLES;
    {
        size_t newest =
            (p->sample_head + ZB_PROGRESS_SAMPLES - 1) % ZB_PROGRESS_SAMPLES;
        if (p->samples[newest].at_ms <= p->samples[oldest].at_ms) {
            return 0.0;
        }
        elapsed_ms = p->samples[newest].at_ms - p->samples[oldest].at_ms;
        if (p->samples[newest].bytes < p->samples[oldest].bytes) {
            return 0.0;
        }
        delta = p->samples[newest].bytes - p->samples[oldest].bytes;
    }
    if (elapsed_ms == 0) {
        return 0.0;
    }
    return (double)delta * 1000.0 / (double)elapsed_ms;
}

static void draw(zb_progress *p)
{
    char transferred[ZB_BYTES_BUF];
    char total_text[ZB_BYTES_BUF];
    char rate_text[ZB_BYTES_BUF + 4];
    char eta_text[ZB_BYTES_BUF];
    double rate = rolling_rate(p);
    int percent = 0;
    int width;
    int bar_width;
    int filled;
    int i;
    zb_buf line;

    if (p->total > 0) {
        double ratio = (double)p->current / (double)p->total;
        if (ratio > 1.0) {
            ratio = 1.0;
        }
        percent = (int)(ratio * 100.0);
    }

    zb_format_bytes(p->current, transferred, sizeof(transferred));
    zb_format_bytes(p->total, total_text, sizeof(total_text));
    zb_format_rate(rate, rate_text, sizeof(rate_text));

    if (rate > 1.0 && p->total > p->current) {
        double remaining = (double)(p->total - p->current) / rate;
        if (remaining < 0.0 || remaining > 1e9) {
            remaining = 0.0;
        }
        zb_format_duration((uint64_t)remaining, eta_text, sizeof(eta_text));
    } else {
        (void)zb_snprintf(eta_text, sizeof(eta_text), "--");
    }

    width = zb_term_width();
    /* Leave room for the numbers; the bar gets whatever is left. */
    bar_width = width - 58;
    if (bar_width < 8) {
        bar_width = 8;
    }
    if (bar_width > 40) {
        bar_width = 40;
    }
    filled = p->total > 0 ? (percent * bar_width) / 100 : 0;

    zb_buf_init(&line);
    (void)zb_buf_append_char(&line, '\r');
    if (p->label != NULL && p->label[0] != '\0') {
        /* Truncate a long filename on a code point boundary so the line never
         * wraps and leaves debris behind. */
        size_t label_budget = 24;
        if (zb_display_width(p->label) > label_budget) {
            size_t cut = zb_utf8_offset(p->label, label_budget - 1);
            (void)zb_buf_append(&line, p->label, cut);
            (void)zb_buf_append_str(&line, "\xe2\x80\xa6 ");
        } else {
            (void)zb_buf_printf(&line, "%s ", p->label);
        }
    }
    (void)zb_buf_append_char(&line, '[');
    for (i = 0; i < bar_width; i++) {
        (void)zb_buf_append_char(&line, i < filled ? '#' : '.');
    }
    (void)zb_buf_printf(&line, "] %3d%%  %s / %s  %s  ETA %s", percent,
                        transferred, total_text, rate_text, eta_text);

    /* Pad to the terminal width so a shorter line overwrites a longer one. */
    {
        size_t drawn_width = zb_display_width(zb_buf_str(&line));
        while (drawn_width < (size_t)width && drawn_width < 512) {
            (void)zb_buf_append_char(&line, ' ');
            drawn_width++;
        }
    }

    fputs(zb_buf_str(&line), stderr);
    fflush(stderr);
    zb_buf_free(&line);
    p->drawn = 1;
}

void zb_progress_update(zb_progress *p, uint64_t current)
{
    uint64_t now;

    if (!p->enabled) {
        return;
    }
    p->current = current;
    now = zb_now_ms();

    p->samples[p->sample_head].at_ms = now;
    p->samples[p->sample_head].bytes = current;
    p->sample_head = (p->sample_head + 1) % ZB_PROGRESS_SAMPLES;
    if (p->sample_count < ZB_PROGRESS_SAMPLES) {
        p->sample_count++;
    }

    /* libcurl calls back far more often than a human can read. */
    if (p->last_draw_ms != 0 &&
        now - p->last_draw_ms < ZB_PROGRESS_MIN_REDRAW_MS) {
        return;
    }
    p->last_draw_ms = now;
    draw(p);
}

void zb_progress_clear(zb_progress *p)
{
    int width;
    int i;

    if (!p->enabled || !p->drawn) {
        return;
    }
    width = zb_term_width();
    fputc('\r', stderr);
    for (i = 0; i < width && i < 512; i++) {
        fputc(' ', stderr);
    }
    fputc('\r', stderr);
    fflush(stderr);
    p->drawn = 0;
}

void zb_progress_finish(zb_progress *p)
{
    char transferred[ZB_BYTES_BUF];
    char rate_text[ZB_BYTES_BUF + 4];
    uint64_t elapsed_ms;

    if (!p->enabled) {
        return;
    }
    zb_progress_clear(p);

    elapsed_ms = zb_now_ms() - p->started_ms;
    zb_format_bytes(p->current, transferred, sizeof(transferred));
    zb_format_rate(elapsed_ms > 0
                       ? (double)p->current * 1000.0 / (double)elapsed_ms
                       : 0.0,
                   rate_text, sizeof(rate_text));

    if (p->label != NULL && p->label[0] != '\0') {
        fprintf(stderr, "%s: %s in ", p->label, transferred);
    } else {
        fprintf(stderr, "%s in ", transferred);
    }
    {
        char duration[ZB_BYTES_BUF];
        zb_format_duration(elapsed_ms / 1000, duration, sizeof(duration));
        fprintf(stderr, "%s (%s)\n", duration, rate_text);
    }
    fflush(stderr);
}
