/** @file
    What a key does under nova-os's US keymap (docs/spec/console.md C3,
    docs/spec/display.md D11, rule 2): the character a US keyboard gives
    for it, or the edit it makes, from the evdev key events the input
    driver sends (input_protocol.h). It is a library so that every
    program that turns key events into text uses the one table. */

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

/** The modifiers held, a bit for each key, and whether Caps Lock is on. */
typedef struct {
    UINT32 held;      /**< a bit for each modifier key held */
    UINT32 caps_lock; /**< nonzero while Caps Lock is on */
} KEYBOARD;

/** What the key event of evdev code and value does, given and updating
    keyboard's modifiers: EDIT_INSERT, with *character set, for a
    character key pressed or repeated, an edit for an editing key, and
    EDIT_NONE for anything else, a release or a modifier among them. */
UINT32 keys_event(KEYBOARD *keyboard, UINT32 code, UINT32 value,
                  UINT8 *character);

#endif
