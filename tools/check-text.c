/* Checks the console's text model and key map (console/text.c,
   console/keys.c, docs/spec/console.md C3 and C4) on the host: writing
   and wrapping, the line editor, the lines kept for scrolling back once
   an edit has wrapped and been taken back, and the US layout with Shift
   and Caps Lock. Prints each failure and a count, and exits nonzero if
   any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I console -o check-text \
     tools/check-text.c console/text.c console/keys.c && ./check-text */

#include <stdio.h>
#include <string.h>

#include "input_protocol.h"
#include "keys.h"
#include "text.h"

#define COLUMNS 10
#define KEY_A 30
#define KEY_1 2
#define KEY_LEFT_SHIFT 42
#define KEY_CAPS_LOCK 58

static TEXT text;
static EDITOR editor;
static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

static int line_is(UINT64 line, const char *want)
{
    char got[TEXT_COLUMNS_MAX + 1];
    UINT32 n = text.columns;

    memcpy(got, text_line(&text, line), n);
    while (n > 0 && got[n - 1] == ' ')
        n--;
    got[n] = '\0';
    return strcmp(got, want) == 0;
}

static void write_text(const char *s)
{
    text_write(&text, (const UINT8 *)s, strlen(s));
}

static void type_text(const char *s)
{
    while (*s)
        editor_key(&editor, &text, EDIT_INSERT, (UINT8)*s++);
}

static void check_writing(void)
{
    text_init(&text, COLUMNS);
    write_text("0123456789ab\tx\r\x01\n");
    check(text.lines == 3, "a line wider than the panel continues");
    check(line_is(0, "0123456789"), "the first ten columns");
    check(line_is(1, "?b      x"),
          "a tab to column 8, CR to the start, a control byte as ?");
}

static void check_editor(void)
{
    UINT64 line;
    UINT32 column;

    text_init(&text, COLUMNS);
    write_text("> ");
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    type_text("acd");
    editor_key(&editor, &text, EDIT_LEFT, 0);
    editor_key(&editor, &text, EDIT_LEFT, 0);
    type_text("b");
    check(line_is(0, "> abcd"), "an insertion after Left");
    editor_caret(&editor, &text, &line, &column);
    check(line == 0 && column == 4, "the caret after the insertion");
    editor_key(&editor, &text, EDIT_END, 0);
    type_text("efghij");
    check(text.lines == 2 && line_is(1, "ij"), "the line wraps as typed");
    editor_key(&editor, &text, EDIT_BACKSPACE, 0);
    editor_key(&editor, &text, EDIT_BACKSPACE, 0);
    check(text.lines == 1 && line_is(0, "> abcdefgh"),
          "Backspace takes the wrapped row back");
    editor_hide(&editor, &text);
    write_text("out\n");
    editor_show(&editor, &text);
    check(line_is(0, "> out") && line_is(1, "abcdefgh"),
          "output goes before the line being edited");
    check(editor_key(&editor, &text, EDIT_ENTER, 0) == 1 &&
              text.lines == 3 && text.column == 0,
          "Enter ends the line and starts the next");
    text_init(&text, COLUMNS);
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 0);
    type_text("secret");
    editor_key(&editor, &text, EDIT_ENTER, 0);
    check(text.lines == 1 && line_is(0, "") && editor.length == 6,
          "a line read without echo is not in the text");
}

/* An edit that wraps after the ring is full writes rows into the slots
   of the oldest lines; taking them back must not count those lines as
   kept again. */
static void check_kept_lines(void)
{
    UINT64 i, first;

    text_init(&text, COLUMNS);
    for (i = 0; i < TEXT_LINES + 5; i++)
        write_text("old\n");
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    type_text("abcdefghijklmnopqrstuvw");
    first = text_first(&text);
    editor_key(&editor, &text, EDIT_HOME, 0);
    for (i = 0; i < 20; i++)
        editor_key(&editor, &text, EDIT_DELETE, 0);
    check(text_first(&text) == first,
          "the oldest line kept does not go back");
    for (i = text_first(&text); i + 1 < text.lines; i++)
        if (!line_is(i, "old")) {
            check(0, "every line kept is whole");
            break;
        }
}

static UINT8 key(KEYBOARD *keyboard, UINT32 code, UINT32 value)
{
    UINT8 character;

    keys_event(keyboard, code, value, &character);
    return character;
}

static void check_keys(void)
{
    KEYBOARD keyboard = {0, 0};

    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'a', "a");
    key(&keyboard, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    check(key(&keyboard, KEY_A, INPUT_KEY_REPEATED) == 'A' &&
              key(&keyboard, KEY_1, INPUT_KEY_PRESSED) == '!',
          "Shift gives A and !");
    key(&keyboard, KEY_LEFT_SHIFT, INPUT_KEY_RELEASED);
    key(&keyboard, KEY_CAPS_LOCK, INPUT_KEY_PRESSED);
    key(&keyboard, KEY_CAPS_LOCK, INPUT_KEY_RELEASED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'A' &&
              key(&keyboard, KEY_1, INPUT_KEY_PRESSED) == '1',
          "Caps Lock shifts the letters alone");
    key(&keyboard, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'a',
          "Shift with Caps Lock unshifts a letter");
}

int main(void)
{
    check_writing();
    check_editor();
    check_kept_lines();
    check_keys();
    printf("%u failures\n", failures);
    return failures != 0;
}
