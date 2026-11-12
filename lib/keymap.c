/* evdev key codes to a US keyboard's characters, and the modifiers held
   (docs/spec/console.md C3, docs/spec/display.md D11, rule 2 and D12).
   The codes are the Linux input header's (docs/spec/README.md,
   Devices), which names each key by its unshifted US label. A modifier
   key's event updates the modifier keys its keyboard holds, which are
   recorded for each keyboard; the modifiers held are those any keyboard
   holds (docs/spec/display.md D19's phase 3 arrangement, rule 6); a
   character key gives its table's character under them; and the secure
   attention key is a Delete with Control and Alt held. */

#include "keymap.h"
#include "display_protocol.h"
#include "input_protocol.h"

/* Key codes this file reads, as the evdev header numbers them. */
#define CODE_BACKSPACE 14
#define CODE_ENTER 28
#define CODE_LEFT_CONTROL 29
#define CODE_LEFT_SHIFT 42
#define CODE_RIGHT_SHIFT 54
#define CODE_LEFT_ALT 56
#define CODE_CAPS_LOCK 58
#define CODE_KEYPAD_DOT 83 /* the keypad's Delete, which the header names
                              by its unshifted label */
#define CODE_KEYPAD_ENTER 96
#define CODE_RIGHT_CONTROL 97
#define CODE_RIGHT_ALT 100
#define CODE_HOME 102
#define CODE_PAGE_UP 104
#define CODE_LEFT 105
#define CODE_RIGHT 106
#define CODE_END 107
#define CODE_PAGE_DOWN 109
#define CODE_DELETE 111
#define CODE_LEFT_SUPER 125
#define CODE_RIGHT_SUPER 126

/* The character keys run from code 2, the 1 key, to code 57, the space
   bar. Each table holds, for every code below CHARACTER_CODES, the
   character the key gives, unshifted and shifted, or NUL: codes 14 and
   15 are Backspace and Tab, 28 and 29 Enter and the left Control, 42 the
   left Shift, and 54 to 56 the right Shift, the keypad's asterisk and
   the left Alt. */
#define CHARACTER_CODES 58

static const char unshifted[] = "\0\0"
                                "1234567890-="
                                "\0\0"
                                "qwertyuiop[]"
                                "\0\0"
                                "asdfghjkl;'`"
                                "\0"
                                "\\zxcvbnm,./"
                                "\0\0\0"
                                " ";
static const char shifted[] = "\0\0"
                              "!@#$%^&*()_+"
                              "\0\0"
                              "QWERTYUIOP{}"
                              "\0\0"
                              "ASDFGHJKL:\"~"
                              "\0"
                              "|ZXCVBNM<>?"
                              "\0\0\0"
                              " ";

_Static_assert(sizeof unshifted == CHARACTER_CODES + 1,
               "a character for every code below CHARACTER_CODES");
_Static_assert(sizeof shifted == CHARACTER_CODES + 1,
               "a character for every code below CHARACTER_CODES");

/* The characters a key can give: the printable ASCII from the space to
   the tilde. */
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E

/* The bits of a keyboard's word of KEYBOARD.held. */
#define HELD_LEFT_SHIFT 0x1
#define HELD_RIGHT_SHIFT 0x2
#define HELD_LEFT_CONTROL 0x4
#define HELD_RIGHT_CONTROL 0x8
#define HELD_LEFT_ALT 0x10
#define HELD_RIGHT_ALT 0x20
#define HELD_LEFT_SUPER 0x40
#define HELD_RIGHT_SUPER 0x80
#define HELD_SHIFT (HELD_LEFT_SHIFT | HELD_RIGHT_SHIFT)
#define HELD_CONTROL (HELD_LEFT_CONTROL | HELD_RIGHT_CONTROL)
#define HELD_ALT (HELD_LEFT_ALT | HELD_RIGHT_ALT)
#define HELD_SUPER (HELD_LEFT_SUPER | HELD_RIGHT_SUPER)
#define HELD_NO_CHARACTER (HELD_CONTROL | HELD_ALT | HELD_SUPER)

/* The bit of a keyboard's word of KEYBOARD.held for a modifier's code, or
   0. */
static UINT32 modifier(UINT32 code)
{
    switch (code) {
    case CODE_LEFT_SHIFT:
        return HELD_LEFT_SHIFT;
    case CODE_RIGHT_SHIFT:
        return HELD_RIGHT_SHIFT;
    case CODE_LEFT_CONTROL:
        return HELD_LEFT_CONTROL;
    case CODE_RIGHT_CONTROL:
        return HELD_RIGHT_CONTROL;
    case CODE_LEFT_ALT:
        return HELD_LEFT_ALT;
    case CODE_RIGHT_ALT:
        return HELD_RIGHT_ALT;
    case CODE_LEFT_SUPER:
        return HELD_LEFT_SUPER;
    case CODE_RIGHT_SUPER:
        return HELD_RIGHT_SUPER;
    default:
        return 0;
    }
}

/* The modifier keys held, by any keyboard: a bit for each key that one of
   them holds. */
static UINT32 held_by_any(const KEYBOARD *keyboard)
{
    UINT32 held = 0, source;

    for (source = 0; source < INPUT_DEVICES_MAX; source++)
        held |= keyboard->held[source];
    return held;
}

/* The edit an editing key makes, or EDIT_NONE for any other code. */
static UINT32 edit_key(UINT32 code)
{
    switch (code) {
    case CODE_BACKSPACE:
        return EDIT_BACKSPACE;
    case CODE_DELETE:
        return EDIT_DELETE;
    case CODE_LEFT:
        return EDIT_LEFT;
    case CODE_RIGHT:
        return EDIT_RIGHT;
    case CODE_HOME:
        return EDIT_HOME;
    case CODE_END:
        return EDIT_END;
    case CODE_ENTER:
    case CODE_KEYPAD_ENTER:
        return EDIT_ENTER;
    case CODE_PAGE_UP:
        return EDIT_PAGE_UP;
    case CODE_PAGE_DOWN:
        return EDIT_PAGE_DOWN;
    default:
        return EDIT_NONE;
    }
}

/* Shift gives a key's shifted character; Caps Lock shifts the letters,
   and with Shift held unshifts them. */
static UINT8 character_of(const KEYBOARD *keyboard, UINT32 code)
{
    char plain = unshifted[code];
    int shift = (held_by_any(keyboard) & HELD_SHIFT) != 0;

    if (plain >= 'a' && plain <= 'z' && keyboard->caps_lock)
        shift = !shift;
    return (UINT8)(shift ? shifted[code] : plain);
}

UINT32 keys_event(KEYBOARD *keyboard, UINT32 source, UINT32 code,
                  UINT32 value, UINT8 *character)
{
    UINT32 bit = modifier(code);

    *character = 0;
    if (bit) {
        if (value == INPUT_KEY_RELEASED)
            keyboard->held[source] &= ~bit;
        else
            keyboard->held[source] |= bit;
        return EDIT_NONE;
    }
    if (value == INPUT_KEY_RELEASED)
        return EDIT_NONE;
    if (code == CODE_CAPS_LOCK) {
        if (value == INPUT_KEY_PRESSED)
            keyboard->caps_lock = !keyboard->caps_lock;
        return EDIT_NONE;
    }
    if (code < CHARACTER_CODES && unshifted[code]) {
        if (held_by_any(keyboard) & HELD_NO_CHARACTER)
            return EDIT_NONE;
        *character = character_of(keyboard, code);
        return EDIT_INSERT;
    }
    return edit_key(code);
}

void keys_gone(KEYBOARD *keyboard, UINT32 source)
{
    keyboard->held[source] = 0;
}

UINT32 keys_window_edit(UINT32 code, UINT32 value, const UINT8 *text,
                        UINT32 text_length, UINT8 *character)
{
    *character = 0;
    if (value == INPUT_KEY_RELEASED)
        return EDIT_NONE;
    if (text_length == 0)
        return edit_key(code);
    if (text_length != 1 || text[0] < PRINTABLE_FIRST ||
        text[0] > PRINTABLE_LAST)
        return EDIT_NONE;
    *character = text[0];
    return EDIT_INSERT;
}

int keys_attention(const KEYBOARD *keyboard, UINT32 code)
{
    UINT32 held = held_by_any(keyboard);

    return (code == CODE_DELETE || code == CODE_KEYPAD_DOT) &&
           (held & HELD_CONTROL) != 0 && (held & HELD_ALT) != 0;
}

UINT32 keys_modifiers(const KEYBOARD *keyboard)
{
    UINT32 held = held_by_any(keyboard);

    return ((held & HELD_SHIFT) ? DISPLAY_MODIFIER_SHIFT : 0) |
           ((held & HELD_CONTROL) ? DISPLAY_MODIFIER_CONTROL : 0) |
           ((held & HELD_ALT) ? DISPLAY_MODIFIER_ALT : 0) |
           ((held & HELD_SUPER) ? DISPLAY_MODIFIER_SUPER : 0);
}
