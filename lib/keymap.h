/** @file
    What a key does under nova-os's US keymap (docs/spec/console.md C3,
    docs/spec/display.md D11, rule 2): the character a US keyboard gives
    for it, or the edit it makes, from the evdev key events the input
    driver sends (input_protocol.h), and whether a key event is the
    secure attention key's (docs/spec/display.md D12). It is a library so
    that every program that reads key events uses the one table and the
    one definition of that key. */

#ifndef NOVA_KEYMAP_H
#define NOVA_KEYMAP_H

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

/** The modifiers held, a bit for each key (Shift, Control, Alt and Super,
    each of the left and right), and whether Caps Lock is on. */
typedef struct {
    UINT32 held;      /**< a bit for each modifier key held */
    UINT32 caps_lock; /**< nonzero while Caps Lock is on */
} KEYBOARD;

/** What the key event of evdev code and value does, given and updating
    keyboard's modifiers: EDIT_INSERT, with *character set, for a
    character key pressed or repeated, an edit for an editing key, and
    EDIT_NONE for anything else, a release or a modifier among them. With
    Control, Alt or Super held a character key gives no character
    (docs/spec/console.md C3); the editing keys still edit. */
UINT32 keys_event(KEYBOARD *keyboard, UINT32 code, UINT32 value,
                  UINT8 *character);

/** Whether the key event of evdev code, of any value (a press, a repeat or
    a release), is of the secure attention key (docs/spec/display.md D12):
    code is Delete or the keypad's Delete, and keyboard holds either
    Control and either Alt, with Shift or Super held or not. It reads the
    modifiers keys_event has recorded and changes nothing; Delete is not a
    modifier, so the answer for its event is the same whether keys_event
    has been given that event yet or not, and a caller gives keys_event
    every event, in order, as well. A caller that is to take the release of
    a press it took remembers the press: the release of Delete after
    Control or Alt was released is not of the key, as the modifiers no
    longer hold. */
int keys_attention(const KEYBOARD *keyboard, UINT32 code);

/** The modifiers keyboard holds, as the DISPLAY_MODIFIER_ bits a window's
    key event carries (docs/spec/display.md D11, rule 2): Shift, Control,
    Alt and Super, each set while either key of its kind is held. */
UINT32 keys_modifiers(const KEYBOARD *keyboard);

#endif
