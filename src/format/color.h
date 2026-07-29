/* color.h — the whole ANSI story. No ncurses, no terminfo.
 *
 * Stateless on purpose: every call takes the resolved `enabled` flag, so there
 * is no process-wide mutable state to get out of sync with --no-color, NO_COLOR
 * or a redirected stdout. */
#ifndef ZB_FORMAT_COLOR_H
#define ZB_FORMAT_COLOR_H

typedef enum {
    ZB_C_RESET = 0,
    ZB_C_BOLD,
    ZB_C_DIM,
    ZB_C_RED,
    ZB_C_GREEN,
    ZB_C_YELLOW,
    ZB_C_BLUE,
    ZB_C_CYAN,
    ZB_C_MAGENTA
} zb_color_id;

/* The escape sequence, or "" when color is disabled. Static storage. */
const char *zb_color(int enabled, zb_color_id id);

/* 1 if color should be used on the given stream, taking NO_COLOR, the
 * --no-color flag (`forced_off`), an explicit config opt-in/out (`configured`:
 * -1 auto, 0 off, 1 on) and whether the stream is a TTY into account. */
int zb_color_should_enable(int is_tty, int forced_off, int configured);

#endif /* ZB_FORMAT_COLOR_H */
