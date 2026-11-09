/* Which lines of the console's text its panel shows (docs/spec/console.md
   C3, C5): the view, which stands some lines back from the text's end,
   how far it scrolls, and the caret's place in it. Pure, so that
   tools/check-text.c checks it and ci/screens.c places the host's
   render of the panel as the console does. */

#ifndef NOVA_VIEW_H
#define NOVA_VIEW_H

#include "text.h"

/* The lines a detent of a pointer's wheel scrolls the view (C3). */
#define VIEW_WHEEL_LINES 3

/* Where the caret is drawn, if lit: its row in the view, and its column,
   from 0 to the text's columns, the last in the panel's padding. */
typedef struct {
    UINT64 row;
    UINT32 column;
    UINT32 lit;
} VIEW_CARET;

/* The lines a turn of a window's wheel scrolls (C2, C3): wheel is the
   turn in DISPLAY_WHEEL_DETENT units of a detent, up positive, and
   *remainder the part of a detent turned and not yet scrolled, which
   starts at 0, keeps the turn's sign and is under a detent. A detent
   scrolls VIEW_WHEEL_LINES lines, so a whole detent a time scrolls just
   as the direct path's detents do, and turns in parts scroll once the
   parts have made a detent. Positive is up, as view_scroll's lines are. */
INT64 view_wheel(INT64 *remainder, INT32 wheel);

/* The most lines a view of rows lines stands back from the text's end,
   where it shows the last rows lines: back to the oldest line kept. */
UINT64 view_back_limit(const TEXT *text, UINT32 rows);

/* Where a view of rows lines that stands back lines back stands once
   scrolled lines further back, or forward for a negative lines: at the
   end or the oldest line kept, if it would pass either. */
UINT64 view_scroll(const TEXT *text, UINT32 rows, UINT64 back, INT64 lines);

/* The first line a view of rows lines shows, standing back lines back. */
UINT64 view_top(const TEXT *text, UINT32 rows, UINT64 back);

/* Where the caret is drawn in a view of rows lines from line top: at
   editor's caret, given while a read waits and the blink shows it, if
   its line is in view; otherwise it is not lit. */
VIEW_CARET view_caret(const EDITOR *editor, const TEXT *text, UINT32 rows,
                      UINT64 top);

#endif
