/* The console's text: the lines it has shown, kept for scrolling back,
   the output position, and the line a read edits (docs/spec/console.md
   C3, C4). Pure: what is drawn is panel.c's. */

#ifndef NOVA_TEXT_H
#define NOVA_TEXT_H

#include "console_protocol.h"

/* The widest line, and the lines kept: the last TEXT_LINES written. */
#define TEXT_COLUMNS_MAX 80
#define TEXT_LINES 1024

/* Line n, of the lines written so far counted from 0, is
   cells[n % TEXT_LINES]; the output position is column of the last,
   from 0 to columns, where columns means the line is full and the next
   character starts a new one. A cell holds printable ASCII. first is
   the oldest line kept whole: it never goes back, since the slots of
   lines before it may hold rows a shown line's edit wrote and took out
   again. */
typedef struct {
    UINT8 cells[TEXT_LINES][TEXT_COLUMNS_MAX];
    UINT64 lines;
    UINT64 first;
    UINT32 columns;
    UINT32 column;
} TEXT;

/* A read's line: its bytes, the caret's index among them, the most it
   takes, whether it is shown, and where it starts, which its bytes
   follow on the screen when it is. */
typedef struct {
    UINT8 bytes[CONSOLE_LINE_MAX];
    UINT32 length;
    UINT32 caret;
    UINT32 limit;
    UINT32 echo;
    UINT64 start_line;
    UINT32 start_column;
} EDITOR;

/* Empty text, one blank line, columns wide, 1 to TEXT_COLUMNS_MAX. */
void text_init(TEXT *text, UINT32 columns);

/* Show count bytes at the output position, as a write does (C4). */
void text_write(TEXT *text, const UINT8 *bytes, UINT64 count);

/* The oldest line kept. */
UINT64 text_first(const TEXT *text);

/* Line's cells, line from text_first to lines - 1. */
const UINT8 *text_line(const TEXT *text, UINT64 line);

/* Start editing a line of at most limit bytes, 1 to CONSOLE_LINE_MAX, at
   the output position, shown as typed if echo is nonzero. */
void editor_start(EDITOR *editor, TEXT *text, UINT32 limit, UINT32 echo);

/* Apply an edit of keymap.h, with its character for EDIT_INSERT. Returns 1
   once EDIT_ENTER ends the line, which moves a shown line's output to
   the next line; 0 otherwise. */
int editor_key(EDITOR *editor, TEXT *text, UINT32 edit, UINT8 character);

/* Take a shown line out of the text, leaving the output position where
   it started, so that output can come before it; editor_show puts it
   back after that output. */
void editor_hide(const EDITOR *editor, TEXT *text);
void editor_show(EDITOR *editor, TEXT *text);

/* Where the caret stands: the line and the column, from 0 to columns. */
void editor_caret(const EDITOR *editor, const TEXT *text, UINT64 *line,
                  UINT32 *column);

#endif
