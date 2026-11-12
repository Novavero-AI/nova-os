/** @file
    What a key does under nova-os's US keymap (docs/spec/console.md C3,
    docs/spec/display.md D11, rule 2): the character a US keyboard gives
    for it, or the edit it makes, from the evdev key events the input
    driver sends (input_protocol.h), and whether a key event is the
    secure attention key's (docs/spec/display.md D12). It is a library so
    that every program that reads key events uses the one table and the
    one definition of that key.

    The keyboards a program reads type as one keyboard
    (docs/spec/display.md D19's phase 3 arrangement, rule 6): a modifier is
    held while any of them holds a key of it, and what each holds is
    recorded apart, so that a keyboard's release, and its going, let go
    its own keys and no other keyboard's. */

#ifndef NOVA_KEYMAP_H
#define NOVA_KEYMAP_H

#include "input_protocol.h"
#include "types.h"

/** @name What a key event does
    The edit keys_event returns.
    @{ */
#define EDIT_NONE 0       /**< no edit: a release, a modifier, any other key */
#define EDIT_INSERT 1     /**< a printable character */
#define EDIT_BACKSPACE 2  /**< Backspace */
#define EDIT_DELETE 3     /**< Delete */
#define EDIT_LEFT 4       /**< Left */
#define EDIT_RIGHT 5      /**< Right */
#define EDIT_HOME 6       /**< Home */
#define EDIT_END 7        /**< End */
#define EDIT_ENTER 8      /**< Enter, or the keypad's */
#define EDIT_PAGE_UP 9    /**< Page Up */
#define EDIT_PAGE_DOWN 10 /**< Page Down */
/** @} */

/** The keyboards a program reads, as the one keyboard they type as: for
    each of them the modifier keys it holds, a bit for each key (Shift,
    Control, Alt and Super, each of the left and right), and whether Caps
    Lock is on, which is one state for them all. A keyboard is numbered by
    its caller, below INPUT_DEVICES_MAX, the most endpoints a client reads
    at once. All zero before the first event. */
typedef struct {
    UINT32 held[INPUT_DEVICES_MAX]; /**< for each keyboard, a bit for each
                                         modifier key it holds */
    UINT32 caps_lock;               /**< nonzero while Caps Lock is on */
} KEYBOARD;

/** What the key event of evdev code and value does, from the keyboard the
    caller numbers source, given and updating keyboard's modifiers:
    EDIT_INSERT, with *character set, for a character key pressed or
    repeated, an edit for an editing key, and EDIT_NONE for anything else,
    a release or a modifier among them. A modifier key pressed or repeated
    is held by source, and its release lets go source's hold of it and no
    other keyboard's. With Control, Alt or Super held, by any keyboard, a
    character key gives no character (docs/spec/console.md C3); the
    editing keys still edit. */
UINT32 keys_event(KEYBOARD *keyboard, UINT32 source, UINT32 code,
                  UINT32 value, UINT8 *character);

/** The keyboard the caller numbers source has gone, as its endpoint's
    close tells: the modifier keys it held are let go, and those another
    keyboard holds stay held. Caps Lock stays as it was. */
void keys_gone(KEYBOARD *keyboard, UINT32 source);

/** What a WINDOW_KEY does (docs/spec/display.md D11, rule 2), the same
    as keys_event gives for the event it was made of: the compositor has
    applied the modifiers and the keymap, so a character key pressed or
    repeated arrives with its text, one printable ASCII character, which
    is EDIT_INSERT with *character set; a release, a key with no text that
    is no editing key, and any text that is not one printable ASCII
    character are EDIT_NONE; an editing key is its edit. The console in a
    window takes its keys by this, since it holds no keyboard's state. */
UINT32 keys_window_edit(UINT32 code, UINT32 value, const UINT8 *text,
                        UINT32 text_length, UINT8 *character);

/** Whether the key event of evdev code, of any value (a press, a repeat or
    a release), is of the secure attention key (docs/spec/display.md D12):
    code is Delete or the keypad's Delete, and either Control and either
    Alt are held, each by any keyboard, with Shift or Super held or not.
    It reads the modifiers keys_event has recorded and changes nothing;
    Delete is not a modifier, so the answer for its event is the same
    whether keys_event has been given that event yet or not, and a caller
    gives keys_event every event, in order, as well. A caller that is to
    take the release of a press it took remembers the press: the release
    of Delete after Control or Alt was released is not of the key, as the
    modifiers no longer hold. */
int keys_attention(const KEYBOARD *keyboard, UINT32 code);

/** The modifiers held, as the DISPLAY_MODIFIER_ bits a window's key event
    carries (docs/spec/display.md D11, rule 2): Shift, Control, Alt and
    Super, each set while either key of its kind is held by any
    keyboard. */
UINT32 keys_modifiers(const KEYBOARD *keyboard);

#endif
