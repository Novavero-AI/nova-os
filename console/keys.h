/* What a key does in the console (docs/spec/console.md C3): the
   character a US keyboard gives for it, or the edit it makes, from the
   evdev key events the input driver sends (input_protocol.h). */

#ifndef NOVA_KEYS_H
#define NOVA_KEYS_H

#include "types.h"

/* What a key event does. */
#define EDIT_NONE 0
#define EDIT_INSERT 1 /* a printable character */
#define EDIT_BACKSPACE 2
#define EDIT_DELETE 3
#define EDIT_LEFT 4
#define EDIT_RIGHT 5
#define EDIT_HOME 6
#define EDIT_END 7
#define EDIT_ENTER 8
#define EDIT_PAGE_UP 9
#define EDIT_PAGE_DOWN 10

/* The modifiers held, a bit for each key, and whether Caps Lock is on. */
typedef struct {
    UINT32 held;
    UINT32 caps_lock;
} KEYBOARD;

/* What the key event of evdev code and value does, given and updating
   keyboard's modifiers: EDIT_INSERT, with *character set, for a
   character key pressed or repeated, an edit for an editing key, and
   EDIT_NONE for anything else, a release or a modifier among them. */
UINT32 keys_event(KEYBOARD *keyboard, UINT32 code, UINT32 value,
                  UINT8 *character);

#endif
