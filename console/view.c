/* The panel's view of the console's text (view.h). */

#include "view.h"
#include "display_protocol.h"

/* The first line of a view of rows lines at the end. */
static UINT64 end_top(const TEXT *text, UINT32 rows)
{
    return text->lines > rows ? text->lines - rows : 0;
}

INT64 view_wheel(INT64 *remainder, INT32 wheel)
{
    INT64 turned = *remainder + wheel;

    *remainder = turned % DISPLAY_WHEEL_DETENT;
    return turned / DISPLAY_WHEEL_DETENT * VIEW_WHEEL_LINES;
}

UINT64 view_back_limit(const TEXT *text, UINT32 rows)
{
    UINT64 end = end_top(text, rows), first = text_first(text);

    return end > first ? end - first : 0;
}

UINT64 view_scroll(const TEXT *text, UINT32 rows, UINT64 back, INT64 lines)
{
    UINT64 limit = view_back_limit(text, rows);
    UINT64 from = back < limit ? back : limit;
    UINT64 by = lines < 0 ? 0 - (UINT64)lines : (UINT64)lines;

    if (lines < 0)
        return by < from ? from - by : 0;
    return by < limit - from ? from + by : limit;
}

UINT64 view_top(const TEXT *text, UINT32 rows, UINT64 back)
{
    UINT64 limit = view_back_limit(text, rows);

    return end_top(text, rows) - (back < limit ? back : limit);
}

VIEW_CARET view_caret(const EDITOR *editor, const TEXT *text, UINT32 rows,
                      UINT64 top)
{
    VIEW_CARET caret;
    UINT64 line;
    UINT32 column;

    caret.row = 0;
    caret.column = 0;
    caret.lit = 0;
    if (!editor)
        return caret;
    editor_caret(editor, text, &line, &column);
    if (line < top || line - top >= rows)
        return caret;
    caret.row = line - top;
    caret.column = column;
    caret.lit = 1;
    return caret;
}
