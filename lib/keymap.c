/* evdev key codes to a US keyboard's characters, and the modifiers held
   (docs/spec/console.md C3, docs/spec/display.md D11, rule 2). The
   codes are the Linux input header's (docs/spec/README.md, Devices),
   which names each key by its unshifted US label. A modifier key's
   event updates the modifiers held, and a character key gives its
   table's character under them. */

#include "keymap.h"
#include "input_protocol.h"

/* Key codes this file reads, as the evdev header numbers them. */
#define CODE_BACKSPACE 14
#define CODE_ENTER 28
#define CODE_LEFT_CONTROL 29
#define CODE_LEFT_SHIFT 42
#define CODE_RIGHT_SHIFT 54
#define CODE_LEFT_ALT 56
#define CODE_CAPS_LOCK 58
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

/* The bits of KEYBOARD.held. */
#define HELD_LEFT_SHIFT 0x1
#define HELD_RIGHT_SHIFT 0x2
#define HELD_LEFT_CONTROL 0x4
#define HELD_RIGHT_CONTROL 0x8
#define HELD_LEFT_ALT 0x10
#define HELD_RIGHT_ALT 0x20
#define HELD_SHIFT (HELD_LEFT_SHIFT | HELD_RIGHT_SHIFT)
#define HELD_NO_CHARACTER                                                 \
    (HELD_LEFT_CONTROL | HELD_RIGHT_CONTROL | HELD_LEFT_ALT | HELD_RIGHT_ALT)

/* The KEYBOARD.held bit for a modifier's code, or 0. */
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
    default:
        return 0;
    }
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
    int shift = (keyboard->held & HELD_SHIFT) != 0;

    if (plain >= 'a' && plain <= 'z' && keyboard->caps_lock)
        shift = !shift;
    return (UINT8)(shift ? shifted[code] : plain);
}

UINT32 keys_event(KEYBOARD *keyboard, UINT32 code, UINT32 value,
                  UINT8 *character)
{
    UINT32 bit = modifier(code);

    *character = 0;
    if (bit) {
        if (value == INPUT_KEY_RELEASED)
            keyboard->held &= ~bit;
        else
            keyboard->held |= bit;
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
        if (keyboard->held & HELD_NO_CHARACTER)
            return EDIT_NONE;
        *character = character_of(keyboard, code);
        return EDIT_INSERT;
    }
    return edit_key(code);
}
