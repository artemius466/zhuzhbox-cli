/* table.h — column alignment for `ls`, `stats` and collection listings.
 *
 * Widths come from zb_display_width(), not strlen, so a CJK filename or an
 * emoji does not knock the columns out of line. */
#ifndef ZB_FORMAT_TABLE_H
#define ZB_FORMAT_TABLE_H

#include <stdio.h>

typedef struct zb_table zb_table;

/* NULL on OOM. */
zb_table *zb_table_new(size_t columns);
void zb_table_free(zb_table *t);

/* All the mutators return 0 on success and -1 on OOM; the table stays valid
 * and freeable after a failure. */
int zb_table_header(zb_table *t, size_t column, const char *text);

/* Right-align a column (for sizes and counts). */
int zb_table_align_right(zb_table *t, size_t column);

/* Begin a new row. Cells default to empty. */
int zb_table_row(zb_table *t);

/* Set the current row's cell. `text` is copied. */
int zb_table_set(zb_table *t, size_t column, const char *text);
int zb_table_setf(zb_table *t, size_t column, const char *fmt, ...);

/* Wrap the current row's cell in an ANSI sequence when the table is printed
 * with color enabled. `escape` must be a static string (e.g. from zb_color()). */
int zb_table_set_style(zb_table *t, size_t column, const char *escape);

/* Print with a two-space gutter. The header row is omitted when it was never
 * set, which is what single-column listings want. */
void zb_table_print(const zb_table *t, FILE *out, int color_enabled);

#endif /* ZB_FORMAT_TABLE_H */
