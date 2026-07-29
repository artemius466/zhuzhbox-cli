#include "format/table.h"

#include <stdarg.h>
#include <string.h>

#include "format/color.h"
#include "util/buf.h"
#include "util/str.h"

struct cell {
    char *text;         /* owned */
    const char *escape; /* static, or NULL */
};

struct row {
    struct cell *cells; /* `columns` of them */
};

struct zb_table {
    size_t columns;
    struct cell *headers;
    int *right_align;
    struct row *rows;
    size_t row_count;
    size_t row_cap;
    int has_header;
};

zb_table *zb_table_new(size_t columns)
{
    zb_table *t;

    if (columns == 0) {
        return NULL;
    }
    t = zb_calloc(1, sizeof(*t));
    if (t == NULL) {
        return NULL;
    }
    t->columns = columns;
    t->headers = zb_calloc(columns, sizeof(*t->headers));
    t->right_align = zb_calloc(columns, sizeof(*t->right_align));
    if (t->headers == NULL || t->right_align == NULL) {
        zb_table_free(t);
        return NULL;
    }
    return t;
}

void zb_table_free(zb_table *t)
{
    size_t i;
    size_t c;

    if (t == NULL) {
        return;
    }
    for (i = 0; i < t->row_count; i++) {
        for (c = 0; c < t->columns; c++) {
            zb_free(t->rows[i].cells[c].text);
        }
        zb_free(t->rows[i].cells);
    }
    zb_free(t->rows);
    if (t->headers != NULL) {
        for (c = 0; c < t->columns; c++) {
            zb_free(t->headers[c].text);
        }
        zb_free(t->headers);
    }
    zb_free(t->right_align);
    zb_free(t);
}

int zb_table_header(zb_table *t, size_t column, const char *text)
{
    char *copy;
    if (t == NULL || column >= t->columns) {
        return -1;
    }
    copy = zb_strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return -1;
    }
    zb_free(t->headers[column].text);
    t->headers[column].text = copy;
    t->has_header = 1;
    return 0;
}

int zb_table_align_right(zb_table *t, size_t column)
{
    if (t == NULL || column >= t->columns) {
        return -1;
    }
    t->right_align[column] = 1;
    return 0;
}

int zb_table_row(zb_table *t)
{
    if (t == NULL) {
        return -1;
    }
    if (t->row_count == t->row_cap) {
        size_t cap = t->row_cap != 0 ? t->row_cap * 2 : 16;
        struct row *grown;
        if (cap > (size_t)-1 / sizeof(*grown)) {
            return -1;
        }
        grown = zb_realloc(t->rows, cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        t->rows = grown;
        t->row_cap = cap;
    }
    t->rows[t->row_count].cells = zb_calloc(t->columns, sizeof(struct cell));
    if (t->rows[t->row_count].cells == NULL) {
        return -1;
    }
    t->row_count++;
    return 0;
}

int zb_table_set(zb_table *t, size_t column, const char *text)
{
    char *copy;
    struct cell *cell;

    if (t == NULL || t->row_count == 0 || column >= t->columns) {
        return -1;
    }
    copy = zb_strdup(text != NULL ? text : "");
    if (copy == NULL) {
        return -1;
    }
    cell = &t->rows[t->row_count - 1].cells[column];
    zb_free(cell->text);
    cell->text = copy;
    return 0;
}

int zb_table_setf(zb_table *t, size_t column, const char *fmt, ...)
{
    va_list ap;
    zb_buf b;
    int rc;

    zb_buf_init(&b);
    va_start(ap, fmt);
    rc = zb_buf_vprintf(&b, fmt, ap);
    va_end(ap);
    if (rc != 0) {
        zb_buf_free(&b);
        return -1;
    }
    rc = zb_table_set(t, column, zb_buf_str(&b));
    zb_buf_free(&b);
    return rc;
}

int zb_table_set_style(zb_table *t, size_t column, const char *escape)
{
    if (t == NULL || t->row_count == 0 || column >= t->columns) {
        return -1;
    }
    t->rows[t->row_count - 1].cells[column].escape = escape;
    return 0;
}

static const char *cell_text(const struct cell *c)
{
    return c->text != NULL ? c->text : "";
}

static void print_padded(FILE *out, const struct cell *c, size_t width,
                         int right, int color_enabled, int is_last)
{
    const char *text = cell_text(c);
    size_t w = zb_display_width(text);
    size_t pad = width > w ? width - w : 0;
    size_t i;
    const char *escape =
        (color_enabled && c->escape != NULL) ? c->escape : "";
    const char *reset = escape[0] != '\0' ? zb_color(1, ZB_C_RESET) : "";

    if (right) {
        for (i = 0; i < pad; i++) {
            fputc(' ', out);
        }
    }
    fputs(escape, out);
    fputs(text, out);
    fputs(reset, out);
    /* Never pad the final column: trailing spaces are noise in a pipe. */
    if (!right && !is_last) {
        for (i = 0; i < pad; i++) {
            fputc(' ', out);
        }
    }
}

void zb_table_print(const zb_table *t, FILE *out, int color_enabled)
{
    size_t *widths;
    size_t c;
    size_t r;

    if (t == NULL) {
        return;
    }
    widths = zb_calloc(t->columns, sizeof(*widths));
    if (widths == NULL) {
        return;
    }

    for (c = 0; c < t->columns; c++) {
        size_t w = t->has_header ? zb_display_width(cell_text(&t->headers[c]))
                                 : 0;
        for (r = 0; r < t->row_count; r++) {
            size_t cw = zb_display_width(cell_text(&t->rows[r].cells[c]));
            if (cw > w) {
                w = cw;
            }
        }
        widths[c] = w;
    }

    if (t->has_header) {
        for (c = 0; c < t->columns; c++) {
            struct cell header = t->headers[c];
            header.escape = zb_color(1, ZB_C_DIM);
            print_padded(out, &header, widths[c], t->right_align[c],
                         color_enabled, c + 1 == t->columns);
            if (c + 1 < t->columns) {
                fputs("  ", out);
            }
        }
        fputc('\n', out);
    }

    for (r = 0; r < t->row_count; r++) {
        for (c = 0; c < t->columns; c++) {
            print_padded(out, &t->rows[r].cells[c], widths[c],
                         t->right_align[c], color_enabled,
                         c + 1 == t->columns);
            if (c + 1 < t->columns) {
                fputs("  ", out);
            }
        }
        fputc('\n', out);
    }

    zb_free(widths);
}
