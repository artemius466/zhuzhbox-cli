#include "format/color.h"

#include "util/buf.h"
#include "util/platform.h"

const char *zb_color(int enabled, zb_color_id id)
{
    if (!enabled) {
        return "";
    }
    switch (id) {
    case ZB_C_RESET:
        return "\033[0m";
    case ZB_C_BOLD:
        return "\033[1m";
    case ZB_C_DIM:
        return "\033[2m";
    case ZB_C_RED:
        return "\033[31m";
    case ZB_C_GREEN:
        return "\033[32m";
    case ZB_C_YELLOW:
        return "\033[33m";
    case ZB_C_BLUE:
        return "\033[34m";
    case ZB_C_CYAN:
        return "\033[36m";
    case ZB_C_MAGENTA:
        return "\033[35m";
    }
    return "";
}

int zb_color_should_enable(int is_tty, int forced_off, int configured)
{
    char *no_color;

    if (forced_off) {
        return 0;
    }
    /* NO_COLOR: any value, even empty, means off. */
    no_color = zb_getenv_dup("NO_COLOR");
    if (no_color != NULL) {
        zb_free(no_color);
        return 0;
    }
    if (configured == 0) {
        return 0;
    }
    if (configured == 1) {
        return 1;
    }
    return is_tty ? 1 : 0;
}
