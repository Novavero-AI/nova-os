/** @file
    How the keyboards' key events reach the window (docs/spec/display.md
    D11, rules 1 and 2, D12, and D19's phase 3 arrangement, rules 3 and
    6): which events are the secure attention key's, which go to no window
    for there is none with the focus, which a window is not sent because
    it was not sent the press they belong to, and what the window is sent
    for the rest, which is a WINDOW_KEY with the modifiers held and the
    text the key gives under the keymap (lib/keymap.c).

    A function decides over the INPUT_EVENT it is given and a KEYROUTE the
    caller keeps, makes no system call, keeps no state of its own and
    allocates nothing, so a host build drives it with any sequence of
    events (tools/check-keyroute.c). Every keyboard the compositor reads
    gives its events to the one KEYROUTE, as every pointing device moves
    the one pointer, so the modifiers held are those of all of them.

    A window is sent a repeat or a release only of a press it was sent, so
    that the keys it believes down are those it was sent the press of:
    KEYROUTE records the presses sent, a bit for each key code. The secure
    attention key is a press of Delete or the keypad's with Control and
    Alt already held; it is sent to no window, so its repeats and its
    release are not sent either, whichever of Control and Alt is held by
    then, and neither are those of a key pressed while no window had the
    focus. A Delete pressed before Control and Alt is not that key: it is
    sent as any other press is, and its repeats and release follow it.

    The attention key's press erases the record of its code, so a window
    that was sent an earlier press of that code and no release of it is
    sent none: its keyboard going costs it the same. */

#ifndef NOVA_KEYROUTE_H
#define NOVA_KEYROUTE_H

#include "input_protocol.h"
#include "keymap.h"
#include "route.h"
#include "types.h"

/** The key codes an event can hold, its code being 16 bits wide. */
#define KEYROUTE_CODES 0x10000

/** The bits of a word of the set of presses sent. */
#define KEYROUTE_WORD_BITS 32

/** What the keyboards have done so far: the modifiers held, and the codes
    whose presses the window was sent and whose releases it has not, a bit
    each. All zero before the first event. */
typedef struct {
    KEYBOARD keyboard; /**< the modifiers held and Caps Lock */
    UINT32 sent[KEYROUTE_CODES / KEYROUTE_WORD_BITS]; /**< the presses
                            sent, bit code % 32 of word code / 32 */
} KEYROUTE;

/** @name What keyroute_event decided
    @{ */
#define KEYROUTE_WINDOW 1    /**< the window is sent *routed */
#define KEYROUTE_NO_WINDOW 2 /**< no window has the focus: the key goes
                                  nowhere */
#define KEYROUTE_ATTENTION 3 /**< the press of the secure attention key:
                                  no window is sent it, and the caller
                                  logs it (D19, rule 8) */
#define KEYROUTE_WITHHELD 4  /**< a repeat or a release of a press the
                                  window was not sent: no window is sent
                                  it and nothing is logged */
/** @} */

/** Take the key event of a keyboard: apply it to the modifiers held, which
    every event does whatever it is decided to be, and decide where it goes.
    focused is nonzero while a window has the focus (D11, rule 1).

    A press of Delete or the keypad's with Control and Alt held, whatever
    else is, is KEYROUTE_ATTENTION, and erases the record of its code's
    press sent. Any other event is KEYROUTE_NO_WINDOW
    where focused is 0. Else a press, of any code, is KEYROUTE_WINDOW and
    is recorded as sent, and a repeat or a release is KEYROUTE_WINDOW if
    the code's press is recorded, a release erasing the record, and
    KEYROUTE_WITHHELD if not. For KEYROUTE_WINDOW, *routed is a WINDOW_KEY
    of the event's code and value and time, the modifiers held once the
    event is applied, and the text the key gives under the keymap: the
    character of a character key pressed or repeated with no Control,
    Alt or Super held, as keys_event gives it, and none for any other
    event. *routed is set only for KEYROUTE_WINDOW. */
UINT32 keyroute_event(KEYROUTE *state, const INPUT_EVENT *event,
                      UINT32 focused, WINDOW_INPUT *routed);

/** A keyboard's endpoint has closed, as a driver's failure closes it: the
    modifiers held are let go, those of the other keyboards too, which are
    one set. Caps Lock stays as it was, and so do the presses sent, which
    the window has had no release of: a release from another keyboard
    reaches it. */
void keyroute_gone(KEYROUTE *state);

/** The window has closed: no press was sent to the next, so none is
    recorded as sent. */
void keyroute_window_gone(KEYROUTE *state);

#endif
