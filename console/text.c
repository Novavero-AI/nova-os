/* The console's text and line editor. A shown line is part of the text
   from its start to the output position, so drawing it needs nothing
   more; an edit takes it out and writes it again, since it holds at
   most CONSOLE_LINE_MAX bytes, a few lines. */

#include "text.h"
#include "keymap.h"

#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E
#define LINE_FEED 0x0A
#define CARRIAGE_RETURN 0x0D
#define TAB 0x09
#define TAB_WIDTH 8
#define BLANK ' '

/* What a byte that is neither printable nor a control the text acts on
   shows as. */
#define REPLACEMENT '?'

static UINT8 *line_cells(TEXT *text, UINT64 line)
{
    return text->cells[line % TEXT_LINES];
}

static void clear_from(TEXT *text, UINT64 line, UINT32 column)
{
    UINT8 *cells = line_cells(text, line);
    UINT32 i;

    for (i = column; i < TEXT_COLUMNS_MAX; i++)
        cells[i] = BLANK;
}

static void new_line(TEXT *text)
{
    text->lines++;
    text->column = 0;
    if (text->lines > TEXT_LINES && text->lines - TEXT_LINES > text->first)
        text->first = text->lines - TEXT_LINES;
    clear_from(text, text->lines - 1, 0);
}

/* A printable byte at the output position, on a new line if the last is
   full. */
static void put(TEXT *text, UINT8 byte)
{
    if (text->column == text->columns)
        new_line(text);
    line_cells(text, text->lines - 1)[text->column++] = byte;
}

static void put_byte(TEXT *text, UINT8 byte)
{
    if (byte == LINE_FEED) {
        new_line(text);
    } else if (byte == CARRIAGE_RETURN) {
        text->column = 0;
    } else if (byte == TAB) {
        do
            put(text, BLANK);
        while (text->column % TAB_WIDTH && text->column < text->columns);
    } else {
        put(text, byte >= PRINTABLE_FIRST && byte <= PRINTABLE_LAST
                      ? byte
                      : (UINT8)REPLACEMENT);
    }
}

void text_init(TEXT *text, UINT32 columns)
{
    text->columns = columns;
    text->lines = 0;
    text->first = 0;
    new_line(text);
}

void text_write(TEXT *text, const UINT8 *bytes, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++)
        put_byte(text, bytes[i]);
}

UINT64 text_first(const TEXT *text)
{
    return text->first;
}

const UINT8 *text_line(const TEXT *text, UINT64 line)
{
    return text->cells[line % TEXT_LINES];
}

/* ------------------------------------------------------------------------
   The line editor. */

/* Back to the line's start, with nothing of it in the text. */
static void unshow(const EDITOR *editor, TEXT *text)
{
    text->lines = editor->start_line + 1;
    text->column = editor->start_column;
    clear_from(text, editor->start_line, editor->start_column);
}

/* Show the line from its start; its start column is below the width, so
   byte i lies start_column + i cells on from the start of its line. */
static void redraw(const EDITOR *editor, TEXT *text)
{
    UINT32 i;

    if (!editor->echo)
        return;
    unshow(editor, text);
    for (i = 0; i < editor->length; i++)
        put(text, editor->bytes[i]);
}

/* Begin the line at the output position, on a new line if the last is
   full. */
static void begin(EDITOR *editor, TEXT *text)
{
    if (editor->echo && text->column == text->columns)
        new_line(text);
    editor->start_line = text->lines - 1;
    editor->start_column = text->column;
}

void editor_start(EDITOR *editor, TEXT *text, UINT32 limit, UINT32 echo)
{
    editor->length = 0;
    editor->caret = 0;
    editor->limit = limit;
    editor->echo = echo;
    begin(editor, text);
}

void editor_hide(const EDITOR *editor, TEXT *text)
{
    if (editor->echo)
        unshow(editor, text);
}

void editor_show(EDITOR *editor, TEXT *text)
{
    begin(editor, text);
    redraw(editor, text);
}

static void insert(EDITOR *editor, UINT8 character)
{
    UINT32 i;

    if (editor->length == editor->limit)
        return;
    for (i = editor->length; i > editor->caret; i--)
        editor->bytes[i] = editor->bytes[i - 1];
    editor->bytes[editor->caret++] = character;
    editor->length++;
}

/* Remove the byte at index, below the length. */
static void remove_at(EDITOR *editor, UINT32 index)
{
    UINT32 i;

    for (i = index; i + 1 < editor->length; i++)
        editor->bytes[i] = editor->bytes[i + 1];
    editor->length--;
}

int editor_key(EDITOR *editor, TEXT *text, UINT32 edit, UINT8 character)
{
    switch (edit) {
    case EDIT_INSERT:
        insert(editor, character);
        break;
    case EDIT_BACKSPACE:
        if (editor->caret == 0)
            return 0;
        remove_at(editor, --editor->caret);
        break;
    case EDIT_DELETE:
        if (editor->caret == editor->length)
            return 0;
        remove_at(editor, editor->caret);
        break;
    case EDIT_LEFT:
        if (editor->caret > 0)
            editor->caret--;
        return 0;
    case EDIT_RIGHT:
        if (editor->caret < editor->length)
            editor->caret++;
        return 0;
    case EDIT_HOME:
        editor->caret = 0;
        return 0;
    case EDIT_END:
        editor->caret = editor->length;
        return 0;
    case EDIT_ENTER:
        if (editor->echo)
            new_line(text);
        return 1;
    default:
        return 0;
    }
    redraw(editor, text);
    return 0;
}

void editor_caret(const EDITOR *editor, const TEXT *text, UINT64 *line,
                  UINT32 *column)
{
    UINT64 offset = (UINT64)editor->start_column + editor->caret;

    if (editor->echo && editor->caret < editor->length) {
        *line = editor->start_line + offset / text->columns;
        *column = (UINT32)(offset % text->columns);
        return;
    }
    *line = text->lines - 1;
    *column = text->column;
}
