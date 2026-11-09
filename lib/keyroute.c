/* How the keyboards' key events reach the window (docs/spec/display.md D11,
   rules 1 and 2, D12 and D19's phase 3 arrangement), as decisions over an
   event's fields and a KEYROUTE. See keyroute.h. */

#include "keyroute.h"
#include "display_protocol.h"

static UINT32 is_sent(const KEYROUTE *state, UINT32 code)
{
    return (state->sent[code / KEYROUTE_WORD_BITS] >>
            (code % KEYROUTE_WORD_BITS)) &
           1u;
}

static void set_sent(KEYROUTE *state, UINT32 code, UINT32 sent)
{
    UINT32 bit = 1u << (code % KEYROUTE_WORD_BITS);

    if (sent)
        state->sent[code / KEYROUTE_WORD_BITS] |= bit;
    else
        state->sent[code / KEYROUTE_WORD_BITS] &= ~bit;
}

/* Bytes, not a structure assignment or initializer: TinyCC turns those
   into calls to memset, and no program here links a C library
   (docs/spec/interface.md P2). */
static void clear(WINDOW_INPUT *input)
{
    UINT8 *bytes = (UINT8 *)input;
    UINT32 i;

    for (i = 0; i < sizeof *input; i++)
        bytes[i] = 0;
}

/* The WINDOW_KEY for event, with the modifiers held and the character it
   gives, which is 0 for none. */
static void make_key(WINDOW_INPUT *routed, const INPUT_EVENT *event,
                     UINT32 modifiers, UINT8 character)
{
    clear(routed);
    routed->kind = WINDOW_KEY;
    routed->body.key.event.code = event->code;
    routed->body.key.event.value = event->value;
    routed->body.key.event.modifiers = modifiers;
    routed->body.key.event.time = event->time;
    if (character) {
        routed->body.key.event.text_length = 1;
        routed->body.key.text[0] = character;
    }
}

/* Whether the window is sent event, which keeps the record of presses
   sent: a press is always sent, and a repeat or a release only if its
   press was. */
static UINT32 window_is_sent(KEYROUTE *state, const INPUT_EVENT *event)
{
    switch (event->value) {
    case INPUT_KEY_PRESSED:
        set_sent(state, event->code, 1);
        return 1;
    case INPUT_KEY_RELEASED:
        if (!is_sent(state, event->code))
            return 0;
        set_sent(state, event->code, 0);
        return 1;
    default:
        return is_sent(state, event->code);
    }
}

UINT32 keyroute_event(KEYROUTE *state, const INPUT_EVENT *event,
                      UINT32 focused, WINDOW_INPUT *routed)
{
    UINT8 character;
    UINT32 pressed_attention =
        event->value == INPUT_KEY_PRESSED &&
        keys_attention(&state->keyboard, event->code);

    keys_event(&state->keyboard, event->code, event->value, &character);
    if (pressed_attention) {
        /* The key now held is the attention key's, whose repeats and
           release go nowhere, so a press of this code the window was sent
           before is forgotten: it is sent no release. */
        set_sent(state, event->code, 0);
        return KEYROUTE_ATTENTION;
    }
    if (!focused)
        return KEYROUTE_NO_WINDOW;
    if (!window_is_sent(state, event))
        return KEYROUTE_WITHHELD;
    make_key(routed, event, keys_modifiers(&state->keyboard), character);
    return KEYROUTE_WINDOW;
}

void keyroute_gone(KEYROUTE *state)
{
    state->keyboard.held = 0;
}

void keyroute_window_gone(KEYROUTE *state)
{
    UINT32 i;

    for (i = 0; i < KEYROUTE_CODES / KEYROUTE_WORD_BITS; i++)
        state->sent[i] = 0;
}
