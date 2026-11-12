/* Checks the console's text model, key map and view (console/text.c,
   lib/keymap.c, console/view.c, docs/spec/console.md C3 to C5) on the
   host: writing and wrapping, the line editor, the lines kept for
   scrolling back once an edit has wrapped and been taken back, the US
   layout with Shift and Caps Lock, which modifiers give no character,
   which hold the secure attention key and which a window is sent, how
   several keyboards type as one, each letting go its own modifier keys
   when it releases them or goes, what
   a window's keys edit, and how far the view scrolls, by the wheel's
   detents, by a window's wheel in parts of a detent and to either end,
   and where it shows the caret.
   Prints each failure and a count, and exits nonzero if any check
   fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I console -I lib \
     -o check-text tools/check-text.c console/text.c lib/keymap.c \
     console/view.c && ./check-text */

#include <stdio.h>
#include <string.h>

#include "display_protocol.h"
#include "input_protocol.h"
#include "keymap.h"
#include "text.h"
#include "view.h"

#define COLUMNS 10

/* The view's lines, fewer than the panel's so that a short text fills
   it, and the lines written for it to scroll through. */
#define ROWS 4
#define WRITTEN 20

#define INT64_LARGEST 0x7fffffffffffffffLL
#define KEY_A 30
#define KEY_1 2
#define KEY_BACKSPACE 14
#define KEY_ENTER 28
#define KEY_LEFT_CONTROL 29
#define KEY_LEFT_SHIFT 42
#define KEY_RIGHT_SHIFT 54
#define KEY_LEFT_ALT 56
#define KEY_SPACE 57
#define KEY_CAPS_LOCK 58
#define KEY_KEYPAD_DOT 83
#define KEY_RIGHT_CONTROL 97
#define KEY_RIGHT_ALT 100
#define KEY_HOME 102
#define KEY_LEFT 105
#define KEY_DELETE 111
#define KEY_LEFT_SUPER 125
#define KEY_RIGHT_SUPER 126

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

/* The keyboards a client reads, each by the number it gives the
   keyboard's endpoint. */
#define FIRST 0
#define SECOND 1

_Static_assert(SECOND < INPUT_DEVICES_MAX, "the checks' keyboards are ones "
                                           "a client reads");

/* The character a key event of the keyboard numbered source gives. */
static UINT8 key_on(KEYBOARD *keyboard, UINT32 source, UINT32 code,
                    UINT32 value)
{
    UINT8 character;

    keys_event(keyboard, source, code, value, &character);
    return character;
}

/* The character a key event of the first keyboard gives. */
static UINT8 key(KEYBOARD *keyboard, UINT32 code, UINT32 value)
{
    return key_on(keyboard, FIRST, code, value);
}

static void check_keys(void)
{
    KEYBOARD keyboard = {{0}, 0};

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
    key(&keyboard, KEY_CAPS_LOCK, INPUT_KEY_REPEATED);
    check(keyboard.caps_lock != 0 &&
              key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "a repeat of Caps Lock does not toggle it");
    key(&keyboard, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'a',
          "Shift with Caps Lock unshifts a letter");
}

/* The modifier keys, one bit of a mask each, in the order the checks press
   them, and the bits of the groups. */
static const UINT32 modifier_keys[] = {
    KEY_LEFT_SHIFT,   KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL,
    KEY_LEFT_ALT,     KEY_RIGHT_ALT,   KEY_LEFT_SUPER,   KEY_RIGHT_SUPER};
#define MODIFIERS (sizeof modifier_keys / sizeof modifier_keys[0])
#define MASKS (1u << MODIFIERS)
#define MASK_SHIFT 0x03u
#define MASK_CONTROL 0x0cu
#define MASK_ALT 0x30u
#define MASK_SUPER 0xc0u

/* How a keyboard comes to hold the modifiers of a mask: by pressing them
   in order, in reverse, or all and then releasing the others. */
typedef enum { BY_ORDER, BY_REVERSE, BY_RELEASE, HOLDINGS } HOLDING;

/* The keyboard numbered source comes to hold the modifiers of mask, by its
   key events alone. */
static void hold_on(KEYBOARD *keyboard, UINT32 source, unsigned mask,
                    HOLDING how)
{
    unsigned i;

    for (i = 0; i < MODIFIERS; i++) {
        unsigned bit = how == BY_REVERSE ? (unsigned)MODIFIERS - 1 - i : i;
        int wanted = (mask >> bit & 1u) != 0;

        if (how == BY_RELEASE || wanted)
            key_on(keyboard, source, modifier_keys[bit], INPUT_KEY_PRESSED);
    }
    if (how == BY_RELEASE)
        for (i = 0; i < MODIFIERS; i++)
            if (!(mask >> i & 1u))
                key_on(keyboard, source, modifier_keys[i],
                       INPUT_KEY_RELEASED);
}

/* A keyboard that holds the modifiers of mask, made by the first keyboard's
   key events alone. */
static KEYBOARD hold(unsigned mask, HOLDING how)
{
    KEYBOARD keyboard = {{0}, 0};

    hold_on(&keyboard, FIRST, mask, how);
    return keyboard;
}

/* The modifiers a window is sent while the modifier keys of mask are held:
   Shift, Control, Alt and Super, each set by either key of its kind. */
static UINT32 window_modifiers(unsigned mask)
{
    return ((mask & MASK_SHIFT) ? DISPLAY_MODIFIER_SHIFT : 0) |
           ((mask & MASK_CONTROL) ? DISPLAY_MODIFIER_CONTROL : 0) |
           ((mask & MASK_ALT) ? DISPLAY_MODIFIER_ALT : 0) |
           ((mask & MASK_SUPER) ? DISPLAY_MODIFIER_SUPER : 0);
}

static const UINT32 values[] = {INPUT_KEY_RELEASED, INPUT_KEY_PRESSED,
                                INPUT_KEY_REPEATED};

/* A window's modifiers are Shift, Control, Alt and Super, each set while
   either key of its kind is held (D11, rule 2): over every subset of the
   eight modifier keys, reached three ways, and unchanged by Caps Lock. */
static void check_modifiers(void)
{
    unsigned mask, how, wrong = 0;

    for (mask = 0; mask < MASKS; mask++)
        for (how = 0; how < HOLDINGS; how++) {
            KEYBOARD keyboard = hold(mask, (HOLDING)how);
            UINT32 want = window_modifiers(mask);

            wrong += keys_modifiers(&keyboard) != want;
            keyboard.caps_lock = 1;
            wrong += keys_modifiers(&keyboard) != want;
        }
    check(wrong == 0, "the modifiers a window is sent are those held, each "
                      "of Shift, Control, Alt and Super set by either key");
}

/* The secure attention key is Delete, or the keypad's, with either Control
   and either Alt held, whatever else is, and for any value: over every
   subset of the eight modifiers, reached three ways, with the answer the
   same before keys_event is given the event and after. */
static void check_attention(void)
{
    static const UINT32 completing[] = {KEY_DELETE, KEY_KEYPAD_DOT};
    static const UINT32 other[] = {
        KEY_A,     KEY_1,         KEY_SPACE,        KEY_ENTER,
        KEY_HOME,  KEY_BACKSPACE, KEY_LEFT,         KEY_CAPS_LOCK,
        KEY_LEFT_CONTROL,         KEY_RIGHT_ALT,    KEY_LEFT_SUPER};
    unsigned mask, how, k, v, wrong = 0, subsets = 0;

    for (mask = 0; mask < MASKS; mask++)
        for (how = 0; how < HOLDINGS; how++) {
            int want = (mask & MASK_CONTROL) != 0 && (mask & MASK_ALT) != 0;
            KEYBOARD keyboard = hold(mask, (HOLDING)how);
            UINT8 character;

            subsets++;
            for (k = 0; k < sizeof completing / sizeof completing[0]; k++)
                for (v = 0; v < sizeof values / sizeof values[0]; v++) {
                    int before = keys_attention(&keyboard, completing[k]);
                    KEYBOARD copy = keyboard;

                    keys_event(&copy, FIRST, completing[k], values[v],
                               &character);
                    wrong += (before != 0) != want ||
                             (keys_attention(&copy, completing[k]) != 0) !=
                                 want;
                }
            for (k = 0; k < sizeof other / sizeof other[0]; k++)
                wrong += keys_attention(&keyboard, other[k]) != 0;
        }
    check(subsets == MASKS * HOLDINGS && wrong == 0,
          "the secure attention key is Delete or the keypad's with Control "
          "and Alt held, whatever else is, for any value");
}

static void check_attention_events(void)
{
    KEYBOARD keyboard = {{0}, 0};

    check(!keys_attention(&keyboard, KEY_DELETE),
          "Delete alone is not the secure attention key");
    key(&keyboard, KEY_LEFT_CONTROL, INPUT_KEY_PRESSED);
    key(&keyboard, KEY_LEFT_ALT, INPUT_KEY_PRESSED);
    check(!keys_attention(&keyboard, KEY_A) &&
              keys_attention(&keyboard, KEY_DELETE),
          "Control and Alt with a key that is not Delete are not the key");
    key(&keyboard, KEY_DELETE, INPUT_KEY_PRESSED);
    key(&keyboard, KEY_DELETE, INPUT_KEY_REPEATED);
    key(&keyboard, KEY_DELETE, INPUT_KEY_RELEASED);
    check(keys_attention(&keyboard, KEY_DELETE),
          "the keyboard still holds Control and Alt after Delete's events");
    key(&keyboard, KEY_LEFT_CONTROL, INPUT_KEY_RELEASED);
    check(!keys_attention(&keyboard, KEY_DELETE),
          "Delete with Control released is not the secure attention key");
}

/* A character key gives its character only with no Control, Alt or Super
   held, shifted by Shift: over every subset of the modifiers, whatever
   Control, Alt and Super are held, and the editing keys edit under all. */
static void check_no_character(void)
{
    static const struct {
        UINT32 code;
        UINT8 plain, shifted;
    } characters[] = {{KEY_A, 'a', 'A'}, {KEY_1, '1', '!'},
                      {KEY_SPACE, ' ', ' '}};
    static const struct {
        UINT32 code, edit;
    } edits[] = {{KEY_BACKSPACE, EDIT_BACKSPACE}, {KEY_DELETE, EDIT_DELETE},
                 {KEY_LEFT, EDIT_LEFT},           {KEY_HOME, EDIT_HOME},
                 {KEY_ENTER, EDIT_ENTER}};
    unsigned mask, how, k, v, wrong = 0;

    for (mask = 0; mask < MASKS; mask++)
        for (how = 0; how < HOLDINGS; how++) {
            int silent = (mask & (MASK_CONTROL | MASK_ALT | MASK_SUPER)) != 0;
            int shift = (mask & MASK_SHIFT) != 0;

            for (k = 0; k < sizeof characters / sizeof characters[0]; k++)
                for (v = 1; v < sizeof values / sizeof values[0]; v++) {
                    KEYBOARD keyboard = hold(mask, (HOLDING)how);
                    UINT8 character;
                    UINT32 edit = keys_event(&keyboard, FIRST,
                                             characters[k].code, values[v],
                                             &character);
                    UINT8 want = silent ? 0
                                 : shift ? characters[k].shifted
                                         : characters[k].plain;

                    wrong += character != want ||
                             edit != (want ? EDIT_INSERT : EDIT_NONE);
                }
            for (k = 0; k < sizeof edits / sizeof edits[0]; k++) {
                KEYBOARD keyboard = hold(mask, (HOLDING)how);
                UINT8 character;

                wrong += keys_event(&keyboard, FIRST, edits[k].code,
                                    INPUT_KEY_PRESSED,
                                    &character) != edits[k].edit ||
                         character != 0;
            }
        }
    check(wrong == 0,
          "a character key gives nothing with Control, Alt or Super held, "
          "and the editing keys edit under every modifier");
}

static void check_super(void)
{
    KEYBOARD keyboard = {{0}, 0};
    UINT8 character;

    check(keys_event(&keyboard, FIRST, KEY_LEFT_SUPER, INPUT_KEY_PRESSED,
                     &character) == EDIT_NONE &&
              character == 0,
          "the Super key gives no text of its own");
    key(&keyboard, KEY_RIGHT_SUPER, INPUT_KEY_PRESSED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 0,
          "with both Super keys held a letter gives nothing");
    key(&keyboard, KEY_LEFT_SUPER, INPUT_KEY_RELEASED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 0,
          "with the right Super key still held a letter gives nothing");
    key(&keyboard, KEY_RIGHT_SUPER, INPUT_KEY_RELEASED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'a',
          "with both Super keys released a letter gives its character");
    key(&keyboard, KEY_CAPS_LOCK, INPUT_KEY_PRESSED);
    key(&keyboard, KEY_CAPS_LOCK, INPUT_KEY_RELEASED);
    key(&keyboard, KEY_LEFT_SUPER, INPUT_KEY_PRESSED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 0,
          "Caps Lock on, Super held, a letter gives nothing");
    key(&keyboard, KEY_LEFT_SUPER, INPUT_KEY_RELEASED);
    check(key(&keyboard, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "Caps Lock on, Super released, a letter is shifted");
}

/* Several keyboards type as one (C3): a modifier held on one combines with
   a key of another, a modifier key that two hold is held until both let
   go, a release lets go the key of the keyboard that sends it, a keyboard
   that goes lets go what it held and nothing another holds, so that
   Control and Alt held on one are the secure attention key's after another
   has sent a key and gone, and Caps Lock is one state that a going leaves
   as it was. */
static void check_keyboards(void)
{
    KEYBOARD keyboard = {{0}, 0};

    key_on(&keyboard, FIRST, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    check(key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "Shift on one keyboard shifts a letter of another");
    key_on(&keyboard, SECOND, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    key_on(&keyboard, FIRST, KEY_LEFT_SHIFT, INPUT_KEY_RELEASED);
    check(key_on(&keyboard, FIRST, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "a modifier key two keyboards hold is held when one releases it");
    key_on(&keyboard, SECOND, KEY_LEFT_SHIFT, INPUT_KEY_RELEASED);
    check(key_on(&keyboard, FIRST, KEY_A, INPUT_KEY_PRESSED) == 'a',
          "and is not held once both have released it");

    key_on(&keyboard, FIRST, KEY_LEFT_CONTROL, INPUT_KEY_PRESSED);
    check(key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 0,
          "Control on one keyboard takes the text of a letter of another");
    key_on(&keyboard, SECOND, KEY_LEFT_CONTROL, INPUT_KEY_RELEASED);
    check(key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 0,
          "a release lets go no key that another keyboard holds");
    key_on(&keyboard, FIRST, KEY_LEFT_CONTROL, INPUT_KEY_RELEASED);
    check(key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 'a',
          "the release of the keyboard that holds a key lets it go");

    key_on(&keyboard, SECOND, KEY_LEFT_CONTROL, INPUT_KEY_PRESSED);
    key_on(&keyboard, SECOND, KEY_RIGHT_ALT, INPUT_KEY_PRESSED);
    key_on(&keyboard, FIRST, KEY_LEFT_SHIFT, INPUT_KEY_PRESSED);
    key_on(&keyboard, FIRST, KEY_A, INPUT_KEY_PRESSED);
    keys_gone(&keyboard, FIRST);
    check(keys_attention(&keyboard, KEY_DELETE) &&
              keys_modifiers(&keyboard) ==
                  (DISPLAY_MODIFIER_CONTROL | DISPLAY_MODIFIER_ALT),
          "Control and Alt held on one keyboard are the secure attention "
          "key's after another has sent a key and gone, and its Shift is "
          "let go");
    keys_gone(&keyboard, SECOND);
    check(!keys_attention(&keyboard, KEY_DELETE) &&
              keys_modifiers(&keyboard) == 0,
          "a keyboard that goes lets go the modifier keys it held");

    key_on(&keyboard, FIRST, KEY_CAPS_LOCK, INPUT_KEY_PRESSED);
    key_on(&keyboard, FIRST, KEY_CAPS_LOCK, INPUT_KEY_RELEASED);
    check(key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "Caps Lock pressed on one keyboard shifts a letter of another");
    keys_gone(&keyboard, FIRST);
    check(keyboard.caps_lock != 0 &&
              key_on(&keyboard, SECOND, KEY_A, INPUT_KEY_PRESSED) == 'A',
          "Caps Lock stays on when the keyboard it was pressed on goes");
}

/* Whether keyboard holds the modifier keys of mask and no other, as the
   modifiers a window is sent, the secure attention key and a letter's
   character show it. */
static int holds(const KEYBOARD *keyboard, unsigned mask)
{
    KEYBOARD copy = *keyboard;
    int attention = (mask & MASK_CONTROL) != 0 && (mask & MASK_ALT) != 0;
    UINT8 letter = (mask & (MASK_CONTROL | MASK_ALT | MASK_SUPER)) ? 0
                   : (mask & MASK_SHIFT)                           ? 'A'
                                                                   : 'a';

    return keys_modifiers(keyboard) == window_modifiers(mask) &&
           (keys_attention(keyboard, KEY_DELETE) != 0) == attention &&
           key_on(&copy, SECOND, KEY_A, INPUT_KEY_PRESSED) == letter;
}

/* The modifiers held are those any keyboard holds, and a keyboard that goes
   lets go its own: over every pair of subsets of the eight modifier keys,
   the first keyboard holding one and the second the other, reached three
   ways in turn, the keys held are those of the two subsets together; with
   either keyboard gone, the other's subset; and with both gone, none. */
static void check_two_keyboards(void)
{
    unsigned first, second, wrong = 0;

    for (first = 0; first < MASKS; first++)
        for (second = 0; second < MASKS; second++) {
            HOLDING how = (HOLDING)((first + second) % HOLDINGS);
            KEYBOARD both = {{0}, 0}, one;

            hold_on(&both, FIRST, first, how);
            hold_on(&both, SECOND, second, how);
            wrong += !holds(&both, first | second);
            one = both;
            keys_gone(&one, FIRST);
            wrong += !holds(&one, second);
            one = both;
            keys_gone(&one, SECOND);
            wrong += !holds(&one, first);
            keys_gone(&one, FIRST);
            wrong += !holds(&one, 0);
        }
    check(wrong == 0,
          "the modifiers held are those either of two keyboards holds, and "
          "a keyboard that goes lets go its own and no other's");
}

/* WRITTEN lines, each ended, and the empty line after them: WRITTEN + 1
   lines in all. */
static void write_lines(void)
{
    UINT64 i;

    text_init(&text, COLUMNS);
    for (i = 0; i < WRITTEN; i++)
        write_text("line\n");
}

static void check_view(void)
{
    UINT64 end = WRITTEN + 1 - ROWS, i;

    text_init(&text, COLUMNS);
    write_text("a\nb\n");
    check(view_back_limit(&text, ROWS) == 0 &&
              view_scroll(&text, ROWS, 0, VIEW_WHEEL_LINES) == 0,
          "a text no longer than the view does not scroll");
    write_lines();
    check(view_back_limit(&text, ROWS) == end &&
              view_top(&text, ROWS, 0) == end,
          "at the end the view shows the last lines, and can go back to "
          "the first");
    check(view_scroll(&text, ROWS, 0, VIEW_WHEEL_LINES) == 3 &&
              view_top(&text, ROWS, 3) == end - 3,
          "a detent up scrolls three lines back");
    check(view_scroll(&text, ROWS, 6, -VIEW_WHEEL_LINES) == 3,
          "a detent down scrolls three lines forward");
    check(view_scroll(&text, ROWS, 1, -VIEW_WHEEL_LINES) == 0,
          "scrolling forward stops at the end");
    check(view_scroll(&text, ROWS, end - 1, VIEW_WHEEL_LINES) == end &&
              view_top(&text, ROWS, end) == 0,
          "scrolling back stops at the first line");
    check(view_scroll(&text, ROWS, end, 5 * VIEW_WHEEL_LINES) == end &&
              view_scroll(&text, ROWS, end, -VIEW_WHEEL_LINES) == end - 3,
          "turned past the first line, a detent down moves the view at "
          "once");
    check(view_top(&text, ROWS, end + 7) == 0 &&
              view_scroll(&text, ROWS, end + 7, -VIEW_WHEEL_LINES) ==
                  end - 3,
          "a view past the first line stands at it");
    check(view_scroll(&text, ROWS, 3, INT64_LARGEST) == end &&
              view_scroll(&text, ROWS, 3, -INT64_LARGEST - 1) == 0,
          "the farthest scrolls in either direction stop at the ends");
    text_init(&text, COLUMNS);
    for (i = 0; i < TEXT_LINES + 5; i++)
        write_text("old\n");
    check(view_top(&text, ROWS,
                   view_scroll(&text, ROWS, 0, (INT64)TEXT_LINES)) ==
              text_first(&text),
          "scrolling back stops at the oldest line kept");
}

static void check_view_caret(void)
{
    VIEW_CARET caret;

    write_lines();
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    type_text("ab");
    caret = view_caret(&editor, &text, ROWS, view_top(&text, ROWS, 0));
    check(caret.lit && caret.row == ROWS - 1 && caret.column == 2,
          "at the end the caret stands on the view's last line");
    caret = view_caret(&editor, &text, ROWS, view_top(&text, ROWS, 1));
    check(!caret.lit, "scrolled back a line, the view hides the caret");
    caret = view_caret(0, &text, ROWS, view_top(&text, ROWS, 0));
    check(!caret.lit, "no caret while no read waits or the blink hides it");
}

/* A turn of a window's wheel, in 120ths of a detent, and what view_wheel
   must give for it from the remainder before: the lines, and the
   remainder after, written out. */
typedef struct {
    INT32 wheel;
    INT64 before, lines, after;
} WHEEL_CASE;

static const WHEEL_CASE wheel_cases[] = {
    {120, 0, 3, 0},
    {-120, 0, -3, 0},
    {240, 0, 6, 0},
    {60, 0, 0, 60},
    {60, 60, 3, 0},
    {-60, -60, -3, 0},
    {119, 0, 0, 119},
    {1, 119, 3, 0},
    {-100, 100, 0, 0},
    {100, -100, 0, 0},
    {-121, 0, -3, -1},
    {-119, -1, -3, 0},
    {130, 0, 3, 10},
    {-130, 10, -3, 0},
    {0, 0, 0, 0},
    {0, 77, 0, 77},
    {0x7fffffff, 0, 3LL * 17895697, 0x7fffffff - 17895697LL * 120},
    {-0x7fffffff - 1, 0, -3LL * 17895697,
     -0x7fffffff - 1 + 17895697LL * 120},
};

#define WHEEL_STEPS 100000
#define WHEEL_LARGEST_TURN 1000

/* A turn of the wheel in whole detents scrolls VIEW_WHEEL_LINES lines each,
   as the direct path's detents do (view_scroll's detent); a turn in parts
   keeps what has not made a detent. Over a long run of turns of either
   sign, the lines scrolled are three to each detent turned and the
   remainder the turns left, so that no turn is lost or counted twice, and
   the remainder stays under a detent. */
static void check_wheel(void)
{
    INT64 remainder = 0, total = 0, scrolled = 0, lines;
    UINT64 state = 0x853c49e6748fea9bULL;
    unsigned i;

    for (i = 0; i < sizeof wheel_cases / sizeof wheel_cases[0]; i++) {
        remainder = wheel_cases[i].before;
        lines = view_wheel(&remainder, wheel_cases[i].wheel);
        check(lines == wheel_cases[i].lines &&
                  remainder == wheel_cases[i].after,
              "a turn of the wheel scrolls the lines written out and "
              "leaves the remainder written out");
    }
    remainder = 0;
    for (i = 0; i < WHEEL_STEPS; i++) {
        INT32 wheel;

        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        wheel = (INT32)((state >> 33) % (2 * WHEEL_LARGEST_TURN + 1)) -
                WHEEL_LARGEST_TURN;
        total += wheel;
        scrolled += view_wheel(&remainder, wheel);
        check(scrolled % VIEW_WHEEL_LINES == 0 &&
                  scrolled / VIEW_WHEEL_LINES * DISPLAY_WHEEL_DETENT +
                          remainder ==
                      total &&
                  remainder > -DISPLAY_WHEEL_DETENT &&
                  remainder < DISPLAY_WHEEL_DETENT,
              "the lines and the remainder account for every turn");
    }
}

int main(void)
{
    check_writing();
    check_editor();
    check_kept_lines();
    check_keys();
    check_no_character();
    check_super();
    check_modifiers();
    check_attention();
    check_attention_events();
    check_keyboards();
    check_two_keyboards();
    check_view();
    check_view_caret();
    check_wheel();
    printf("%u failures\n", failures);
    return failures != 0;
}
