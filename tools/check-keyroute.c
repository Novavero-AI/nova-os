/* Host check of lib/keyroute.c, how the keyboards' key events reach the
   window: docs/spec/display.md D11, rules 1 and 2 (the keyboard goes to
   the window with the focus, a key event carries the code, the value, the
   modifiers held and the text the key gives), D12 (the secure attention
   key is sent to no window) and D19's phase 3 arrangement, rules 3, 6 and
   8.

   Four kinds of case. A table of sequences, each event with the decision
   and the window's event written out by hand, covers the edges: the
   attention key with Control and Alt, with Shift too, on the keypad, with
   its release after Control's, with repeats, and two presses of it at
   once; the same keys in other orders, which are no attention key; a press
   of a code whose press was the attention key's, which is a press of its
   own; the attention key's press of a code the window was sent an earlier
   press of, on one keyboard and on two that share the state; repeats and
   releases of presses the window was not sent; no window with the focus; a
   keyboard that goes with keys held, and a window that goes; and each
   character key's text. Random sessions of events from the keys that
   matter, the focus coming and going, run beside a model written another
   way (the modifier keys held kept by code, a flag for each code whose
   press the window was sent and whose release it was not, the text from a
   layout table of its own, and the whole event the window is sent built as
   bytes), and every code's text is compared with it too. The model keeps
   what the library keeps, so it can share a fault of the algorithm; the
   third kind cannot. Laws of the decisions, read back over the events a
   session has decided with no record of presses sent: the attention key's
   press is decided as such exactly when Delete or the keypad's is pressed
   with Control and Alt held; a press is sent to the window with the focus
   and to no window else; a repeat or a release is sent only if the last
   event of its code the window was sent is a press or a repeat, the window
   has not closed since and no press of the code was the attention key's
   since; and one that is not is not sent. And laws of the stream: an
   event that is not sent leaves what it was given untouched, and each
   event the window is sent encodes as a message that display.desc's
   generated check accepts, and decodes to the fields it was made from.

   The key a console in a window takes (lib/keymap.c, keys_window_edit,
   console.md C2 and C3) is checked beside them: a table of events with
   the edit and character written out by hand, and a law over random
   sessions, that the edit of each WINDOW_KEY the window is sent is the edit
   keys_event gives the same events given in order to a keyboard of its own,
   which is the console's direct path, with the same character.

   Random cases come from SEEDS seeds, a number of sessions from each, and
   a failure names its seed: the same one every run. Another run is
   -DSEEDS= or -DSESSIONS_PER_SEED= on the command line.

   cc -std=c11 -I include -I lib -I gfx tools/check-keyroute.c \
      lib/keyroute.c lib/keymap.c */

#include <stdio.h>
#include <string.h>

#include "display_protocol.h"
#include "keyroute.h"

#define FAILURES_SHOWN 12

/* The seeds are splitmix64's outputs for SEED_BASE and the steps after it:
   a seed's state is the mix of the base advanced n + 1 increments, so that
   neighbouring seeds share nothing. */
#define SEED_BASE 0x2545F4914F6CDD1Dull
#define SPLITMIX_INCREMENT 0x9E3779B97F4A7C15ull
#define SPLITMIX_FIRST 0xBF58476D1CE4E5B9ull
#define SPLITMIX_SECOND 0x94D049BB133111EBull
#define NO_SEED (~0u)

#ifndef SEEDS
#define SEEDS 24
#endif
#ifndef SESSIONS_PER_SEED
#define SESSIONS_PER_SEED 400
#endif
#define SESSION_STEPS 240

/* evdev's key codes, as the Linux input header numbers them. */
#define KEY_ESC 1
#define KEY_1 2
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_Q 16
#define KEY_P 25
#define KEY_LEFT_BRACE 26
#define KEY_RIGHT_BRACE 27
#define KEY_ENTER 28
#define KEY_LEFT_CONTROL 29
#define KEY_A 30
#define KEY_C 46
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE 41
#define KEY_LEFT_SHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z 44
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_RIGHT_SHIFT 54
#define KEY_KEYPAD_ASTERISK 55
#define KEY_LEFT_ALT 56
#define KEY_SPACE 57
#define KEY_CAPS_LOCK 58
#define KEY_F1 59
#define KEY_KEYPAD_DOT 83
#define KEY_KEYPAD_ENTER 96
#define KEY_RIGHT_CONTROL 97
#define KEY_RIGHT_ALT 100
#define KEY_HOME 102
#define KEY_PAGE_UP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_PAGE_DOWN 109
#define KEY_DELETE 111
#define KEY_LEFT_SUPER 125
#define KEY_RIGHT_SUPER 126
#define KEY_MAX 0x2ff
#define CODE_GREATEST 0xFFFF

#define PRESS INPUT_KEY_PRESSED
#define RELEASE INPUT_KEY_RELEASED
#define REPEAT INPUT_KEY_REPEATED

#define TIME_STEP 977u
#define UNTOUCHED 0xA5

static unsigned failures;
static unsigned long long checks;
static unsigned seed_number = NO_SEED;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (passed || failures++ >= FAILURES_SHOWN)
        return;
    printf("failed: %s (%lld, %lld)", what, a, b);
    if (seed_number != NO_SEED)
        printf(" in seed %u", seed_number);
    printf("\n");
}

static unsigned long long random_state;

static void seed_random(unsigned n)
{
    unsigned long long z = SEED_BASE + (n + 1ull) * SPLITMIX_INCREMENT;

    z = (z ^ (z >> 30)) * SPLITMIX_FIRST;
    z = (z ^ (z >> 27)) * SPLITMIX_SECOND;
    random_state = z ^ (z >> 31);
}

static unsigned long long next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static unsigned long long below(unsigned long long limit)
{
    return next_random() % limit;
}

static INPUT_EVENT make_event(UINT32 code, UINT32 value, UINT64 time)
{
    INPUT_EVENT e;

    memset(&e, 0, sizeof e);
    e.type = INPUT_TYPE_KEY;
    e.code = (UINT16)code;
    e.value = value;
    e.time = time;
    return e;
}

/* The event a window is sent, as bytes: a WINDOW_KEY of these fields, with
   text, one character or none, and every other byte 0. */
static WINDOW_INPUT make_input(UINT32 code, UINT32 value, UINT32 modifiers,
                               UINT64 time, char text)
{
    WINDOW_INPUT in;

    memset(&in, 0, sizeof in);
    in.kind = WINDOW_KEY;
    in.body.key.event.code = (UINT16)code;
    in.body.key.event.value = value;
    in.body.key.event.modifiers = modifiers;
    in.body.key.event.time = time;
    in.body.key.event.text_length = text != 0;
    in.body.key.text[0] = (UINT8)text;
    return in;
}

/* A WINDOW_INPUT that the library has not been asked to fill. */
static WINDOW_INPUT untouched(void)
{
    WINDOW_INPUT in;

    memset(&in, UNTOUCHED, sizeof in);
    return in;
}

/* ------------------------------------------------------------------------
   The model */

/* The modifier keys, each with the DISPLAY_MODIFIER_ bit of its kind. */
typedef struct {
    UINT32 code;
    UINT32 bit;
} MODIFIER_KEY;

static const MODIFIER_KEY modifier_keys[] = {
    {KEY_LEFT_SHIFT, DISPLAY_MODIFIER_SHIFT},
    {KEY_RIGHT_SHIFT, DISPLAY_MODIFIER_SHIFT},
    {KEY_LEFT_CONTROL, DISPLAY_MODIFIER_CONTROL},
    {KEY_RIGHT_CONTROL, DISPLAY_MODIFIER_CONTROL},
    {KEY_LEFT_ALT, DISPLAY_MODIFIER_ALT},
    {KEY_RIGHT_ALT, DISPLAY_MODIFIER_ALT},
    {KEY_LEFT_SUPER, DISPLAY_MODIFIER_SUPER},
    {KEY_RIGHT_SUPER, DISPLAY_MODIFIER_SUPER}};
#define MODIFIER_KEYS (sizeof modifier_keys / sizeof modifier_keys[0])

typedef struct {
    int held[MODIFIER_KEYS]; /* by modifier_keys' order */
    int caps;
    char sent[CODE_GREATEST + 1]; /* by code: the window has been sent
                                     its press and not its release */
} MODEL;

static UINT32 model_modifiers(const MODEL *model)
{
    UINT32 bits = 0;
    unsigned i;

    for (i = 0; i < MODIFIER_KEYS; i++)
        if (model->held[i])
            bits |= modifier_keys[i].bit;
    return bits;
}

/* The character keys of a US keyboard, as docs/spec/console.md C3 has
   them: a key's evdev name is its unshifted label. Each row is the keys
   from code first on, with what each gives unshifted and shifted. */
typedef struct {
    UINT32 first;
    const char *plain;
    const char *shifted;
} LAYOUT_ROW;

static const LAYOUT_ROW layout[] = {
    {KEY_1, "1234567890-=", "!@#$%^&*()_+"},
    {KEY_Q, "qwertyuiop[]", "QWERTYUIOP{}"},
    {KEY_A, "asdfghjkl;'`", "ASDFGHJKL:\"~"},
    {KEY_BACKSLASH, "\\", "|"},
    {KEY_Z, "zxcvbnm,./", "ZXCVBNM<>?"},
    {KEY_SPACE, " ", " "}};
#define LAYOUT_ROWS (sizeof layout / sizeof layout[0])

/* The character a US keyboard gives for code, 0 for a key with none: shift
   gives the shifted character, and caps shifts a letter alone, so that the
   two together give it unshifted. */
static char model_character(UINT32 code, int shift, int caps)
{
    unsigned row;

    for (row = 0; row < LAYOUT_ROWS; row++) {
        const LAYOUT_ROW *keys = &layout[row];

        if (code >= keys->first && code - keys->first < strlen(keys->plain)) {
            UINT32 at = code - keys->first;
            int letter = keys->plain[at] >= 'a' && keys->plain[at] <= 'z';

            return (letter ? shift != caps : shift) ? keys->shifted[at]
                                                    : keys->plain[at];
        }
    }
    return 0;
}

/* What the model says of an event: the decision and, for a window's, the
   event it is sent. */
typedef struct {
    UINT32 decision;
    WINDOW_INPUT input;
} WANT;

static void model_step(MODEL *model, UINT32 code, UINT32 value, UINT64 time,
                       UINT32 focused, WANT *want)
{
    UINT32 before = model_modifiers(model), after;
    int attention, text_allowed;
    unsigned i;

    attention = value == PRESS &&
                (code == KEY_DELETE || code == KEY_KEYPAD_DOT) &&
                (before & DISPLAY_MODIFIER_CONTROL) &&
                (before & DISPLAY_MODIFIER_ALT);
    for (i = 0; i < MODIFIER_KEYS; i++)
        if (modifier_keys[i].code == code)
            model->held[i] = value != RELEASE;
    if (code == KEY_CAPS_LOCK && value == PRESS)
        model->caps = !model->caps;
    after = model_modifiers(model);
    want->input = untouched();
    if (attention) {
        model->sent[code] = 0;
        want->decision = KEYROUTE_ATTENTION;
    } else if (!focused) {
        want->decision = KEYROUTE_NO_WINDOW;
    } else if (value != PRESS && !model->sent[code]) {
        want->decision = KEYROUTE_WITHHELD;
    } else {
        model->sent[code] = value != RELEASE;
        text_allowed = value != RELEASE &&
                       !(after & (DISPLAY_MODIFIER_CONTROL |
                                  DISPLAY_MODIFIER_ALT |
                                  DISPLAY_MODIFIER_SUPER));
        want->decision = KEYROUTE_WINDOW;
        want->input = make_input(
            code, value, after, time,
            text_allowed ? model_character(
                               code, (after & DISPLAY_MODIFIER_SHIFT) != 0,
                               model->caps)
                         : 0);
    }
}

static void model_gone(MODEL *model)
{
    memset(model->held, 0, sizeof model->held);
}

static void model_window_gone(MODEL *model)
{
    memset(model->sent, 0, sizeof model->sent);
}

/* ------------------------------------------------------------------------
   The laws of what a window is sent */

/* The window's event as its channel carries it: encoded, checked by the
   generated check, and decoded again to the fields it was made from. */
static void check_wire(const WINDOW_INPUT *in)
{
    static union {
        UINT64 alignment;
        UINT8 bytes[DISPLAY_WINDOW_TO_CLIENT_BYTES_MAX];
    } wire;
    UINT32 size = 0;
    UINT32 encoded = display_window_key_encode(
        &in->body.key.event, in->body.key.text,
        in->body.key.event.text_length, wire.bytes, &size);
    UINT32 which = 0;
    const WINDOW_KEY_EVENT *back = (const WINDOW_KEY_EVENT *)wire.bytes;

    check(encoded == OK, "an event sent encodes", encoded, 0);
    if (encoded != OK)
        return;
    check(display_window_event_check(wire.bytes, size, 0, 0, &which) == OK &&
              which == WINDOW_KEY,
          "the generated check takes it as a key", which, 0);
    check(back->code == in->body.key.event.code &&
              back->value == in->body.key.event.value &&
              back->modifiers == in->body.key.event.modifiers &&
              back->time == in->body.key.event.time &&
              back->text_length == in->body.key.event.text_length &&
              memcmp(display_window_key_characters(wire.bytes),
                     in->body.key.text, back->text_length) == 0,
          "it decodes to the fields it was made from", back->code, 0);
}

/* ------------------------------------------------------------------------
   A table of sequences */

typedef struct {
    UINT16 code;
    UINT16 value;
    UINT8 focused;
    UINT8 decision;
    UINT8 modifiers;
    char text;
} STEP;

/* The markers for a step that is the keyboard's going, or the window's,
   not an event. */
#define GONE 0xFFFF
#define WINDOW_GONE 0xFFFE

#define CTRL DISPLAY_MODIFIER_CONTROL
#define ALT DISPLAY_MODIFIER_ALT
#define SHIFT DISPLAY_MODIFIER_SHIFT
#define SENT KEYROUTE_WINDOW
#define NOWIN KEYROUTE_NO_WINDOW
#define ATTN KEYROUTE_ATTENTION
#define WITHHELD KEYROUTE_WITHHELD

/* Each sequence runs from a fresh KEYROUTE, with the focus as each step
   says. */
static const STEP typing[] = {
    {KEY_A, PRESS, 1, SENT, 0, 'a'},
    {KEY_A, REPEAT, 1, SENT, 0, 'a'},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
    {KEY_LEFT_SHIFT, PRESS, 1, SENT, SHIFT, 0},
    {KEY_A, PRESS, 1, SENT, SHIFT, 'A'},
    {KEY_1, PRESS, 1, SENT, SHIFT, '!'},
    {KEY_LEFT_SHIFT, RELEASE, 1, SENT, 0, 0},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
    {KEY_SPACE, PRESS, 1, SENT, 0, ' '},
    {KEY_LEFT, PRESS, 1, SENT, 0, 0},
    {KEY_ENTER, PRESS, 1, SENT, 0, 0},
    {KEY_F1, PRESS, 1, SENT, 0, 0},
    {KEY_CAPS_LOCK, PRESS, 1, SENT, 0, 0},
    {KEY_CAPS_LOCK, REPEAT, 1, SENT, 0, 0},
    {KEY_CAPS_LOCK, RELEASE, 1, SENT, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'A'},
    {KEY_RIGHT_SHIFT, PRESS, 1, SENT, SHIFT, 0},
    {KEY_A, PRESS, 1, SENT, SHIFT, 'a'},
    {KEY_1, PRESS, 1, SENT, SHIFT, '!'},
    {KEY_MAX, PRESS, 1, SENT, SHIFT, 0},
    {CODE_GREATEST, PRESS, 1, SENT, SHIFT, 0},
    {CODE_GREATEST, RELEASE, 1, SENT, SHIFT, 0},
};

/* A press and its release of one key, with the modifiers held and the text
   the press gives. */
#define TAP(code, modifiers, text)             \
    {code, PRESS, 1, SENT, modifiers, text},   \
    {code, RELEASE, 1, SENT, modifiers, 0}

/* The keys whose characters a letter's rule does not reach, unshifted, with
   Shift, and with Caps Lock, which shifts the letters alone. */
static const STEP punctuation[] = {
    TAP(KEY_MINUS, 0, '-'),       TAP(KEY_EQUAL, 0, '='),
    TAP(KEY_LEFT_BRACE, 0, '['),  TAP(KEY_RIGHT_BRACE, 0, ']'),
    TAP(KEY_SEMICOLON, 0, ';'),   TAP(KEY_APOSTROPHE, 0, '\''),
    TAP(KEY_GRAVE, 0, '`'),       TAP(KEY_BACKSLASH, 0, '\\'),
    TAP(KEY_COMMA, 0, ','),       TAP(KEY_DOT, 0, '.'),
    TAP(KEY_SLASH, 0, '/'),       TAP(KEY_TAB, 0, 0),
    TAP(KEY_KEYPAD_ASTERISK, 0, 0),
    {KEY_LEFT_SHIFT, PRESS, 1, SENT, SHIFT, 0},
    TAP(KEY_MINUS, SHIFT, '_'),   TAP(KEY_EQUAL, SHIFT, '+'),
    TAP(KEY_LEFT_BRACE, SHIFT, '{'), TAP(KEY_RIGHT_BRACE, SHIFT, '}'),
    TAP(KEY_SEMICOLON, SHIFT, ':'), TAP(KEY_APOSTROPHE, SHIFT, '"'),
    TAP(KEY_GRAVE, SHIFT, '~'),   TAP(KEY_BACKSLASH, SHIFT, '|'),
    TAP(KEY_COMMA, SHIFT, '<'),   TAP(KEY_DOT, SHIFT, '>'),
    TAP(KEY_SLASH, SHIFT, '?'),   TAP(KEY_SPACE, SHIFT, ' '),
    {KEY_LEFT_SHIFT, RELEASE, 1, SENT, 0, 0},
    {KEY_CAPS_LOCK, PRESS, 1, SENT, 0, 0},
    {KEY_CAPS_LOCK, RELEASE, 1, SENT, 0, 0},
    TAP(KEY_MINUS, 0, '-'),       TAP(KEY_BACKSLASH, 0, '\\'),
    TAP(KEY_SLASH, 0, '/'),       TAP(KEY_Q, 0, 'Q'),
};

/* A repeat or a release is sent only for a press that was: none before the
   press, none for a second release, and none after the release. */
static const STEP only_of_presses_sent[] = {
    {KEY_A, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'a'},
    {KEY_A, REPEAT, 1, SENT, 0, 'a'},
    {KEY_A, REPEAT, 1, SENT, 0, 'a'},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
    {KEY_A, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'a'},
    {KEY_LEFT_SHIFT, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
    {CODE_GREATEST, RELEASE, 1, WITHHELD, 0, 0},
    {CODE_GREATEST, REPEAT, 1, WITHHELD, 0, 0},
    {0, RELEASE, 1, WITHHELD, 0, 0},
};

static const STEP combinations[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_C, PRESS, 1, SENT, CTRL, 0},
    {KEY_C, RELEASE, 1, SENT, CTRL, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_RIGHT_ALT, PRESS, 1, SENT, ALT, 0},
    {KEY_A, PRESS, 1, SENT, ALT, 0},
    {KEY_RIGHT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_RIGHT_SUPER, PRESS, 1, SENT, DISPLAY_MODIFIER_SUPER, 0},
    {KEY_A, PRESS, 1, SENT, DISPLAY_MODIFIER_SUPER, 0},
    {KEY_RIGHT_SUPER, RELEASE, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_RIGHT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, CTRL, 0},
    {KEY_A, PRESS, 1, SENT, CTRL, 0},
    {KEY_RIGHT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'a'},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_DELETE, PRESS, 1, SENT, CTRL, 0},
    {KEY_DELETE, RELEASE, 1, SENT, CTRL, 0},
};

/* The keys of a keyboard whose releases come in the order of its presses,
   as QEMU's send-key gives them. */
static const STEP attention_released_in_order[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, ALT, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_DELETE, RELEASE, 1, SENT, 0, 0},
};

static const STEP attention_keypad_shifted[] = {
    {KEY_LEFT_SHIFT, PRESS, 1, SENT, SHIFT, 0},
    {KEY_RIGHT_CONTROL, PRESS, 1, SENT, SHIFT | CTRL, 0},
    {KEY_RIGHT_ALT, PRESS, 1, SENT, SHIFT | CTRL | ALT, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, ATTN, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_RIGHT_ALT, RELEASE, 1, SENT, SHIFT | CTRL, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, SENT, SHIFT | CTRL, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, SENT, SHIFT | CTRL, 0},
};

static const STEP attention_two_at_once[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
};

/* Delete first, then Control and Alt: not the key. Nor is Control and
   Delete, nor Alt and Delete. */
static const STEP attention_other_orders[] = {
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, REPEAT, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, CTRL, 0},
    {KEY_DELETE, PRESS, 1, SENT, CTRL, 0},
    {KEY_DELETE, RELEASE, 1, SENT, CTRL, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, ALT, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, SENT, ALT, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, SENT, ALT, 0},
};

/* The record of presses sent is by code: while the attention keys' presses
   are not sent, the codes beside them, a word of the set away, and a bit
   past the top of the 16 bits of a code, are, with their repeats and
   releases, and the attention keys' repeats and releases are not. */
static const STEP sent_is_by_code[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE - 1, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE + 1, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE - 32, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE + 32, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE ^ 0x8000, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT - 32, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT + 32, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT ^ 0x4000, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT ^ 0x10, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE ^ 0x10, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_DELETE - 1, REPEAT, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE + 1, REPEAT, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE - 32, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE + 32, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE ^ 0x8000, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT - 32, REPEAT, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT + 32, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT ^ 0x4000, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT ^ 0x10, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE ^ 0x10, RELEASE, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, WITHHELD, 0, 0},
};

/* A press of a code whose last press was the attention key's is a press of
   its own, decided as any press is: the attention key again where Control
   and Alt are held, a press for the window where they are not, with its
   repeats and its release sent, and none for no window, whose repeat and
   release are not sent either. */
static const STEP presses_are_fresh[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, ALT, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_DELETE, REPEAT, 1, SENT, 0, 0},
    {KEY_DELETE, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, ATTN, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, ALT, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_KEYPAD_DOT, PRESS, 0, NOWIN, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, WITHHELD, 0, 0},
};

/* The attention key's press of a code the window was sent an earlier press
   of, and not the release of, erases it: the repeats and the release of the
   key that is held now are not sent, and the window is sent none for the
   earlier press, as it is sent none when its keyboard goes. The keyboard of
   the earlier press goes with the key held; then the keypad's, which a
   press with no Control or Alt held first sends; and then a press while no
   window has the focus, which erases it as well. */
static const STEP attention_erases_a_press_sent[] = {
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {GONE, 0, 0, 0, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, ALT, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, SENT, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, SENT, 0, 0},
    {KEY_RIGHT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_RIGHT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_KEYPAD_DOT, PRESS, 1, ATTN, 0, 0},
    {KEY_KEYPAD_DOT, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_KEYPAD_DOT, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_RIGHT_ALT, RELEASE, 1, SENT, CTRL, 0},
    {KEY_RIGHT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 0, ATTN, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
};

/* Two keyboards give their events to the one state. The first holds Delete,
   which the window was sent; the second presses Control, Alt and Delete,
   which is the attention key whichever keyboard holds each of its keys, and
   its repeats and its release, and the first keyboard's release of the key
   it holds, are not sent. The first keyboard then goes, with the second's
   Control and Alt cleared with it, and the second's release of them is
   sent. */
static const STEP two_keyboards_one_state[] = {
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_RIGHT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {GONE, 0, 0, 0, 0, 0},
    {KEY_DELETE, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_RIGHT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_DELETE, RELEASE, 1, SENT, 0, 0},
};

/* No window has the focus: the keys go nowhere but are still held, and the
   attention key is the key all the same. */
static const STEP no_window[] = {
    {KEY_A, PRESS, 0, NOWIN, 0, 0},
    {KEY_LEFT_SHIFT, PRESS, 0, NOWIN, 0, 0},
    {KEY_A, RELEASE, 0, NOWIN, 0, 0},
    {KEY_A, PRESS, 1, SENT, SHIFT, 'A'},
    {KEY_A, RELEASE, 1, SENT, SHIFT, 0},
    {KEY_LEFT_SHIFT, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 0, NOWIN, 0, 0},
    {KEY_LEFT_ALT, PRESS, 0, NOWIN, 0, 0},
    {KEY_DELETE, PRESS, 0, ATTN, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_ALT, RELEASE, 1, WITHHELD, 0, 0},
};

/* A keyboard that goes with Control, Alt and the attention key held: its
   modifiers are not held, and Caps Lock stays on. The presses the window
   was sent stay sent, so that a release of one from another keyboard
   reaches it, and the attention key's press, which it was not sent, has no
   release sent. */
static const STEP keyboard_gone[] = {
    {KEY_CAPS_LOCK, PRESS, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_ALT, PRESS, 1, SENT, CTRL | ALT, 0},
    {KEY_DELETE, PRESS, 1, ATTN, 0, 0},
    {GONE, 0, 0, 0, 0, 0},
    {KEY_DELETE, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, SENT, 0, 0},
    {KEY_LEFT_ALT, RELEASE, 1, SENT, 0, 0},
    {KEY_DELETE, PRESS, 1, SENT, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'A'},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_DELETE, PRESS, 1, SENT, CTRL, 0},
};

/* A window that goes: the presses it was sent are forgotten, and none of
   their releases is sent to the next. The modifiers held stay held. */
static const STEP window_gone[] = {
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {KEY_A, PRESS, 1, SENT, CTRL, 0},
    {WINDOW_GONE, 0, 0, 0, 0, 0},
    {KEY_A, REPEAT, 1, WITHHELD, 0, 0},
    {KEY_A, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, PRESS, 1, SENT, 0, 'a'},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
    {KEY_LEFT_CONTROL, PRESS, 1, SENT, CTRL, 0},
    {WINDOW_GONE, 0, 0, 0, 0, 0},
    {KEY_A, PRESS, 1, SENT, CTRL, 0},
    {KEY_LEFT_CONTROL, RELEASE, 1, WITHHELD, 0, 0},
    {KEY_A, RELEASE, 1, SENT, 0, 0},
};

typedef struct {
    const char *name;
    const STEP *steps;
    unsigned count;
} SEQUENCE;

#define SEQUENCE_OF(name, steps) \
    {name, steps, sizeof steps / sizeof steps[0]}

static const SEQUENCE sequences[] = {
    SEQUENCE_OF("typing", typing),
    SEQUENCE_OF("punctuation and the text of every character key",
                punctuation),
    SEQUENCE_OF("a repeat or a release only of a press sent",
                only_of_presses_sent),
    SEQUENCE_OF("combinations", combinations),
    SEQUENCE_OF("attention, released in the order pressed",
                attention_released_in_order),
    SEQUENCE_OF("attention, on the keypad with Shift",
                attention_keypad_shifted),
    SEQUENCE_OF("two attention presses at once", attention_two_at_once),
    SEQUENCE_OF("other orders", attention_other_orders),
    SEQUENCE_OF("the record of presses sent is by code", sent_is_by_code),
    SEQUENCE_OF("a press of an attention key's code is a press of its own",
                presses_are_fresh),
    SEQUENCE_OF("the attention key erases a press sent of its code",
                attention_erases_a_press_sent),
    SEQUENCE_OF("two keyboards on one state", two_keyboards_one_state),
    SEQUENCE_OF("no window", no_window),
    SEQUENCE_OF("a keyboard that goes", keyboard_gone),
    SEQUENCE_OF("a window that goes", window_gone),
};

static void check_sequence(const SEQUENCE *sequence)
{
    static KEYROUTE state;
    unsigned i;

    memset(&state, 0, sizeof state);
    for (i = 0; i < sequence->count; i++) {
        const STEP *step = &sequence->steps[i];
        UINT64 time = (UINT64)(i + 1) * TIME_STEP;
        INPUT_EVENT event = make_event(step->code, step->value, time);
        WINDOW_INPUT routed = untouched(), want = untouched();
        UINT32 got;

        if (step->code == GONE) {
            keyroute_gone(&state);
            continue;
        }
        if (step->code == WINDOW_GONE) {
            keyroute_window_gone(&state);
            continue;
        }
        got = keyroute_event(&state, &event, step->focused, &routed);
        check(got == step->decision, sequence->name, i, got);
        if (step->decision == KEYROUTE_WINDOW) {
            want = make_input(step->code, step->value, step->modifiers, time,
                              step->text);
            check(memcmp(&routed, &want, sizeof want) == 0,
                  "the event the window is sent is as written", i, 0);
        } else {
            check(memcmp(&routed, &want, sizeof want) == 0,
                  "an event not sent leaves *routed as it was", i, 0);
        }
    }
}

static void check_tables(void)
{
    unsigned i;

    for (i = 0; i < sizeof sequences / sizeof sequences[0]; i++)
        check_sequence(&sequences[i]);
}

/* ------------------------------------------------------------------------
   The record of presses sent, a bit to a code */

/* Codes at the ends and the middle of a word of the record, and at the ends
   of the record. */
static const UINT32 bit_codes[] = {0,          1,          31,     32,
                                   KEY_DELETE, KEY_KEYPAD_DOT, 0x7FFF,
                                   0x8000,     0xFFE0,     CODE_GREATEST};

#define BIT_CODES (sizeof bit_codes / sizeof bit_codes[0])

/* How many codes the state has the press of recorded as sent, seen as a
   repeat of each is decided: a code recorded gives KEYROUTE_WINDOW to the
   focused window and any other KEYROUTE_WITHHELD, and a repeat changes the
   modifiers held and no record. *unrecorded is the last code that is not. */
static unsigned count_sent(KEYROUTE *state, UINT32 *unrecorded)
{
    unsigned sent = 0;
    UINT32 code;

    for (code = 0; code <= CODE_GREATEST; code++) {
        INPUT_EVENT event = make_event(code, REPEAT, TIME_STEP);
        WINDOW_INPUT routed = untouched();

        if (keyroute_event(state, &event, 1, &routed) == KEYROUTE_WINDOW)
            sent++;
        else
            *unrecorded = code;
    }
    return sent;
}

/* Control and Alt held on a state, as the attention key needs. */
static void hold_control_and_alt(KEYROUTE *state)
{
    INPUT_EVENT control = make_event(KEY_LEFT_CONTROL, PRESS, TIME_STEP);
    INPUT_EVENT alt = make_event(KEY_LEFT_ALT, PRESS, TIME_STEP);
    WINDOW_INPUT routed = untouched();

    keyroute_event(state, &control, 1, &routed);
    keyroute_event(state, &alt, 1, &routed);
}

/* So that each code's bit is seen alone: from a state no sequence of
   events reaches, every press recorded, a release gives up its own bit and
   no other, the attention key's press gives up its own bit and no other, a
   press gives up none, and a keyboard's going gives up none, which only the
   window's going does; and from an empty state, a press records its own
   bit and no other. */
static void check_sent_bits(void)
{
    static KEYROUTE state;
    unsigned i;
    UINT32 unrecorded = 0;

    for (i = 0; i < BIT_CODES; i++) {
        INPUT_EVENT release = make_event(bit_codes[i], RELEASE, TIME_STEP);
        INPUT_EVENT press = make_event(bit_codes[i], PRESS, TIME_STEP);
        WINDOW_INPUT routed = untouched();

        memset(&state, 0, sizeof state);
        memset(state.sent, 0xFF, sizeof state.sent);
        check(keyroute_event(&state, &release, 1, &routed) == KEYROUTE_WINDOW,
              "the release of a press sent is sent", bit_codes[i], 0);
        check(count_sent(&state, &unrecorded) == CODE_GREATEST &&
                  unrecorded == bit_codes[i],
              "a release gives up the bit of its code alone", bit_codes[i],
              unrecorded);

        memset(&state, 0, sizeof state);
        check(keyroute_event(&state, &press, 1, &routed) == KEYROUTE_WINDOW,
              "a press is sent", bit_codes[i], 0);
        check(count_sent(&state, &unrecorded) == 1 &&
                  unrecorded != bit_codes[i],
              "a press records the bit of its code alone", bit_codes[i],
              unrecorded);

        memset(&state, 0, sizeof state);
        hold_control_and_alt(&state);
        memset(state.sent, 0xFF, sizeof state.sent);
        if (bit_codes[i] == KEY_DELETE || bit_codes[i] == KEY_KEYPAD_DOT) {
            check(keyroute_event(&state, &press, 1, &routed) ==
                      KEYROUTE_ATTENTION,
                  "a press of Delete or the keypad's with Control and Alt "
                  "held is the attention key's",
                  bit_codes[i], 0);
            check(count_sent(&state, &unrecorded) == CODE_GREATEST &&
                      unrecorded == bit_codes[i],
                  "the attention key's press gives up the bit of its code "
                  "alone",
                  bit_codes[i], unrecorded);
        } else {
            check(keyroute_event(&state, &press, 1, &routed) ==
                      KEYROUTE_WINDOW,
                  "any other press with Control and Alt held is sent",
                  bit_codes[i], 0);
            check(count_sent(&state, &unrecorded) == CODE_GREATEST + 1u,
                  "a press gives up no bit", bit_codes[i], 0);
        }
    }

    memset(&state, 0, sizeof state);
    memset(state.sent, 0xFF, sizeof state.sent);
    keyroute_gone(&state);
    check(count_sent(&state, &unrecorded) == CODE_GREATEST + 1u,
          "a keyboard's going gives up no press sent", 0, 0);
    keyroute_window_gone(&state);
    check(count_sent(&state, &unrecorded) == 0,
          "a window's going gives up every press sent", 0, 0);
}

/* ------------------------------------------------------------------------
   The laws of the decisions

   Each reads back over what a session has done, as the spec words it, and
   keeps no record of presses sent: where the library's record and the
   model's flags could share a fault, these cannot. */

/* What a session has done at a step: an event, with whether a window had
   the focus and what was decided for it, a keyboard's going, or the
   window's. */
#define LOGGED_EVENT 0
#define LOGGED_KEYBOARD_GONE 1
#define LOGGED_WINDOW_GONE 2

typedef struct {
    UINT8 kind;
    UINT8 value;
    UINT8 focused;
    UINT8 decision;
    UINT16 code;
} LOGGED;

/* Whether the key of code is held at step now: its latest event says so,
   whatever was decided for it, and a repeat holds it as a press does,
   unless a keyboard has gone since. */
static int log_is_held(const LOGGED *log, unsigned now, UINT32 code)
{
    unsigned i;

    for (i = now; i-- > 0;) {
        if (log[i].kind == LOGGED_KEYBOARD_GONE)
            return 0;
        if (log[i].kind == LOGGED_EVENT && log[i].code == code)
            return log[i].value != RELEASE;
    }
    return 0;
}

/* The value of the last event of code the window was sent before step now,
   or -1 if it was sent none or has closed since. */
static int log_last_sent(const LOGGED *log, unsigned now, UINT32 code)
{
    unsigned i;

    for (i = now; i-- > 0;) {
        if (log[i].kind == LOGGED_WINDOW_GONE)
            return -1;
        if (log[i].kind == LOGGED_EVENT && log[i].code == code &&
            log[i].decision == KEYROUTE_WINDOW)
            return log[i].value;
    }
    return -1;
}

/* Whether the latest press of code before step now that was decided to be
   the attention key's or the window's, was the attention key's. */
static int log_attention_since(const LOGGED *log, unsigned now, UINT32 code)
{
    unsigned i;

    for (i = now; i-- > 0;)
        if (log[i].kind == LOGGED_EVENT && log[i].code == code) {
            if (log[i].decision == KEYROUTE_ATTENTION)
                return 1;
            if (log[i].value == PRESS && log[i].decision == KEYROUTE_WINDOW)
                return 0;
        }
    return 0;
}

/* The laws of the event at step now, the log's last. */
static void check_laws(const LOGGED *log, unsigned now)
{
    const LOGGED *event = &log[now];
    UINT32 code = event->code, value = event->value;
    int attention = value == PRESS &&
                    (code == KEY_DELETE || code == KEY_KEYPAD_DOT) &&
                    (log_is_held(log, now, KEY_LEFT_CONTROL) ||
                     log_is_held(log, now, KEY_RIGHT_CONTROL)) &&
                    (log_is_held(log, now, KEY_LEFT_ALT) ||
                     log_is_held(log, now, KEY_RIGHT_ALT));
    int last, since;

    check((event->decision == KEYROUTE_ATTENTION) == attention,
          "the attention key's press is Delete or the keypad's pressed with "
          "Control and Alt held, and no other event is",
          code, value);
    if (attention)
        return;
    if (!event->focused) {
        check(event->decision == KEYROUTE_NO_WINDOW,
              "with no window with the focus an event goes nowhere", code,
              value);
        return;
    }
    if (value == PRESS) {
        check(event->decision == KEYROUTE_WINDOW,
              "a press that is not the attention key's goes to the window "
              "with the focus",
              code, 0);
        return;
    }
    last = log_last_sent(log, now, code);
    since = log_attention_since(log, now, code);
    if (event->decision == KEYROUTE_WINDOW) {
        check(last == PRESS || last == REPEAT,
              "a repeat or a release is sent only if the last event of its "
              "code the window was sent was a press or a repeat, and the "
              "window has not closed since",
              code, last);
        check(!since,
              "a repeat or a release is not sent after the attention key's "
              "press of its code until a later press of it is sent",
              code, value);
    } else {
        check(event->decision == KEYROUTE_WITHHELD &&
                  !((last == PRESS || last == REPEAT) && !since),
              "a repeat or a release is withheld only where one of the "
              "laws forbids it",
              code, last);
    }
}

/* ------------------------------------------------------------------------
   Random sessions */

/* The codes a session's events come from: every key the model knows the
   text of or the library treats specially, one for no key at all, and, a
   tenth of the time, any code the event can hold. */
static UINT32 random_code(void)
{
    static const UINT32 pool[] = {
        KEY_ESC,         KEY_1,           KEY_0,       KEY_BACKSPACE,
        KEY_Q,           KEY_P,           KEY_ENTER,   KEY_LEFT_CONTROL,
        KEY_A,           KEY_C,           KEY_L,       KEY_LEFT_SHIFT,
        KEY_Z,           KEY_M,           KEY_RIGHT_SHIFT, KEY_LEFT_ALT,
        KEY_SPACE,       KEY_CAPS_LOCK,   KEY_F1,      KEY_KEYPAD_DOT,
        KEY_KEYPAD_ENTER, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_HOME,
        KEY_LEFT,        KEY_DELETE,      KEY_LEFT_SUPER, KEY_RIGHT_SUPER,
        KEY_DELETE,      KEY_KEYPAD_DOT,  KEY_LEFT_CONTROL, KEY_LEFT_ALT,
        KEY_MINUS,       KEY_EQUAL,       KEY_LEFT_BRACE, KEY_RIGHT_BRACE,
        KEY_SEMICOLON,   KEY_APOSTROPHE,  KEY_GRAVE,   KEY_BACKSLASH,
        KEY_COMMA,       KEY_DOT,         KEY_SLASH,    KEY_TAB,
        KEY_MAX,         0};

    if (below(10) == 0)
        return (UINT32)below(CODE_GREATEST + 1);
    return pool[below(sizeof pool / sizeof pool[0])];
}

static UINT32 random_value(void)
{
    UINT64 pick = below(100);

    return pick < 45 ? PRESS : pick < 85 ? RELEASE : REPEAT;
}

/* What a seed's sessions reached, counted so that each is known to reach
   every decision, a window's text, and a keyboard's going and a window's. */
static unsigned reached[KEYROUTE_WITHHELD + 1];
static unsigned texts, keyboards_gone, windows_gone;

/* An event given to the library and to the model: the decision and the
   event the window is sent are compared with the model's, and so are the
   modifiers held and Caps Lock. Returns the decision; *routed is what the
   library made. */
static UINT32 step_and_compare(KEYROUTE *state, MODEL *model, UINT32 code,
                               UINT32 value, UINT64 time, UINT32 focused,
                               WINDOW_INPUT *routed)
{
    INPUT_EVENT event = make_event(code, value, time);
    UINT32 got;
    WANT want;

    *routed = untouched();
    model_step(model, code, value, time, focused, &want);
    got = keyroute_event(state, &event, focused, routed);
    check(got == want.decision, "the decision is the model's", got,
          want.decision);
    if (got == KEYROUTE_WINDOW)
        check(memcmp(routed, &want.input, sizeof *routed) == 0,
              "the event the window is sent is the model's", code, value);
    check(keys_modifiers(&state->keyboard) == model_modifiers(model) &&
              (state->keyboard.caps_lock != 0) == (model->caps != 0),
          "the modifiers held and Caps Lock are the model's", code, value);
    return got;
}

static void check_session(void)
{
    static KEYROUTE state;
    static MODEL model;
    static LOGGED log[SESSION_STEPS];
    UINT32 focused = 1;
    unsigned step;

    memset(&state, 0, sizeof state);
    memset(&model, 0, sizeof model);
    memset(log, 0, sizeof log);
    for (step = 0; step < SESSION_STEPS; step++) {
        UINT64 time = (UINT64)(step + 1) * TIME_STEP;
        UINT32 code, value, got;
        WINDOW_INPUT routed;

        if (below(40) == 0)
            focused = !focused;
        if (below(70) == 0) {
            keyroute_gone(&state);
            model_gone(&model);
            log[step].kind = LOGGED_KEYBOARD_GONE;
            keyboards_gone++;
            continue;
        }
        if (below(90) == 0) {
            keyroute_window_gone(&state);
            model_window_gone(&model);
            log[step].kind = LOGGED_WINDOW_GONE;
            focused = 0;
            windows_gone++;
            continue;
        }
        code = random_code();
        value = random_value();
        got = step_and_compare(&state, &model, code, value, time, focused,
                               &routed);
        log[step].kind = LOGGED_EVENT;
        log[step].code = (UINT16)code;
        log[step].value = (UINT8)value;
        log[step].focused = (UINT8)focused;
        log[step].decision = (UINT8)got;
        reached[got]++;
        if (got == KEYROUTE_WINDOW) {
            check_wire(&routed);
            texts += routed.body.key.event.text_length;
        } else {
            WINDOW_INPUT same = untouched();

            check(memcmp(&routed, &same, sizeof same) == 0,
                  "an event not sent leaves *routed as it was", code, value);
        }
        check_laws(log, step);
    }
}

static void check_seed(unsigned n)
{
    unsigned session;

    memset(reached, 0, sizeof reached);
    texts = keyboards_gone = windows_gone = 0;
    seed_number = n;
    seed_random(n);
    for (session = 0; session < SESSIONS_PER_SEED; session++)
        check_session();
    check(reached[KEYROUTE_WINDOW] > SESSIONS_PER_SEED &&
              reached[KEYROUTE_NO_WINDOW] > SESSIONS_PER_SEED &&
              reached[KEYROUTE_ATTENTION] > SESSIONS_PER_SEED &&
              reached[KEYROUTE_WITHHELD] > SESSIONS_PER_SEED &&
              texts > SESSIONS_PER_SEED &&
              keyboards_gone > SESSIONS_PER_SEED &&
              windows_gone > SESSIONS_PER_SEED,
          "a seed's sessions reach every decision, text, and a keyboard's "
          "going and a window's",
          reached[KEYROUTE_ATTENTION], reached[KEYROUTE_WITHHELD]);
    seed_number = NO_SEED;
}

static void check_sessions(void)
{
    unsigned n;

    for (n = 0; n < SEEDS; n++)
        check_seed(n);
}

/* ------------------------------------------------------------------------
   The text of every code */

/* Whether code is a key the model keeps as a modifier or a lock, which a
   press and a release would leave changed. */
static int is_modifier_or_lock(UINT32 code)
{
    unsigned i;

    for (i = 0; i < MODIFIER_KEYS; i++)
        if (modifier_keys[i].code == code)
            return 1;
    return code == KEY_CAPS_LOCK;
}

/* Every code but the modifiers and Caps Lock, pressed and released with
   neither Shift nor Caps Lock on, each, and both: the window is sent what
   the model says, which is the text of the layout of console.md C3 for each
   of its keys and none for any other. The codes that gave text must be
   every key of the layout once in each of the four states. */
static void check_text_of_every_code(void)
{
    static KEYROUTE state;
    static MODEL model;
    WINDOW_INPUT routed;
    unsigned held, row, layout_keys = 0, with_text = 0;
    UINT32 code;

    for (row = 0; row < LAYOUT_ROWS; row++)
        layout_keys += (unsigned)strlen(layout[row].plain);
    for (held = 0; held < 4; held++) {
        memset(&state, 0, sizeof state);
        memset(&model, 0, sizeof model);
        if (held & 1)
            step_and_compare(&state, &model, KEY_LEFT_SHIFT, PRESS, TIME_STEP,
                             1, &routed);
        if (held & 2)
            step_and_compare(&state, &model, KEY_CAPS_LOCK, PRESS, TIME_STEP,
                             1, &routed);
        for (code = 0; code <= CODE_GREATEST; code++) {
            if (is_modifier_or_lock(code))
                continue;
            step_and_compare(&state, &model, code, PRESS, TIME_STEP, 1,
                             &routed);
            with_text += routed.body.key.event.text_length == 1 &&
                         routed.kind == WINDOW_KEY;
            step_and_compare(&state, &model, code, RELEASE, TIME_STEP, 1,
                             &routed);
        }
    }
    check(with_text == 4 * layout_keys,
          "every key of the layout gives text, under each of Shift and Caps "
          "Lock, and no other key does",
          with_text, 4 * layout_keys);
}

/* ------------------------------------------------------------------------
   The key a console in a window takes */

/* An event's text as a WINDOW_KEY carries it and the edit and character
   keys_window_edit must give, written out: the editing keys of console.md
   C3's table by code, a printable character of one byte, and nothing for
   a release, a modifier, a key that gives no text, or text that is not one
   printable ASCII character. */
typedef struct {
    UINT32 code, value;
    const char *text;
    UINT32 text_length;
    UINT32 edit;
    UINT8 character;
} WINDOW_EDIT_CASE;

static const WINDOW_EDIT_CASE window_edits[] = {
    {KEY_A, PRESS, "a", 1, EDIT_INSERT, 'a'},
    {KEY_A, REPEAT, "A", 1, EDIT_INSERT, 'A'},
    {KEY_SPACE, PRESS, " ", 1, EDIT_INSERT, ' '},
    {KEY_GRAVE, PRESS, "~", 1, EDIT_INSERT, '~'},
    {KEY_A, RELEASE, "a", 1, EDIT_NONE, 0},
    {KEY_A, PRESS, "", 0, EDIT_NONE, 0},
    {KEY_A, PRESS, "ab", 2, EDIT_NONE, 0},
    {KEY_A, PRESS, "\x7f", 1, EDIT_NONE, 0},
    {KEY_A, PRESS, "\x1f", 1, EDIT_NONE, 0},
    {KEY_A, PRESS, "\xc3", 1, EDIT_NONE, 0},
    {KEY_LEFT_SHIFT, PRESS, "", 0, EDIT_NONE, 0},
    {KEY_CAPS_LOCK, PRESS, "", 0, EDIT_NONE, 0},
    {KEY_F1, PRESS, "", 0, EDIT_NONE, 0},
    {KEY_MAX, PRESS, "", 0, EDIT_NONE, 0},
    {KEY_BACKSPACE, PRESS, "", 0, EDIT_BACKSPACE, 0},
    {KEY_DELETE, PRESS, "", 0, EDIT_DELETE, 0},
    {KEY_LEFT, REPEAT, "", 0, EDIT_LEFT, 0},
    {KEY_RIGHT, PRESS, "", 0, EDIT_RIGHT, 0},
    {KEY_HOME, PRESS, "", 0, EDIT_HOME, 0},
    {KEY_END, PRESS, "", 0, EDIT_END, 0},
    {KEY_ENTER, PRESS, "", 0, EDIT_ENTER, 0},
    {KEY_KEYPAD_ENTER, PRESS, "", 0, EDIT_ENTER, 0},
    {KEY_PAGE_UP, PRESS, "", 0, EDIT_PAGE_UP, 0},
    {KEY_PAGE_DOWN, REPEAT, "", 0, EDIT_PAGE_DOWN, 0},
    {KEY_LEFT, RELEASE, "", 0, EDIT_NONE, 0},
    {KEY_ENTER, RELEASE, "", 0, EDIT_NONE, 0},
};

static void check_window_edit_table(void)
{
    unsigned i;

    for (i = 0; i < sizeof window_edits / sizeof window_edits[0]; i++) {
        const WINDOW_EDIT_CASE *c = &window_edits[i];
        UINT8 character = UNTOUCHED;
        UINT32 edit = keys_window_edit(c->code, c->value,
                                       (const UINT8 *)c->text, c->text_length,
                                       &character);

        check(edit == c->edit, "a window's key makes the edit written out",
              c->code, edit);
        check(character == c->character,
              "a window's key gives the character written out", c->code,
              character);
    }
}

/* The codes of the sessions below: every editing key, the keys that give
   text, the modifiers, and keys that do nothing. */
static const UINT32 window_edit_codes[] = {
    KEY_ESC,        KEY_1,           KEY_A,         KEY_Z,
    KEY_SPACE,      KEY_BACKSPACE,   KEY_ENTER,     KEY_KEYPAD_ENTER,
    KEY_HOME,       KEY_END,         KEY_LEFT,      KEY_RIGHT,
    KEY_DELETE,     KEY_KEYPAD_DOT,  KEY_PAGE_UP,   KEY_PAGE_DOWN,
    KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_ALT,
    KEY_LEFT_ALT,   KEY_LEFT_SUPER,  KEY_CAPS_LOCK, KEY_F1,
    KEY_GRAVE,      KEY_SLASH,       KEY_TAB,       KEY_MAX};

/* Each event of random sessions goes to a KEYROUTE, as the compositor
   takes it, and to a keyboard of its own, as the console takes it in
   direct mode; for the events the window is sent, keys_window_edit of the
   WINDOW_KEY is the edit and character keys_event gives. */
static void check_window_edit_session(unsigned *edits, unsigned *inserts)
{
    static KEYROUTE state;
    static KEYBOARD direct;
    unsigned step;

    memset(&state, 0, sizeof state);
    memset(&direct, 0, sizeof direct);
    for (step = 0; step < SESSION_STEPS; step++) {
        INPUT_EVENT event = make_event(
            window_edit_codes[below(sizeof window_edit_codes /
                                    sizeof window_edit_codes[0])],
            random_value(), (UINT64)(step + 1) * TIME_STEP);
        UINT8 direct_character, window_character;
        UINT32 direct_edit =
            keys_event(&direct, event.code, event.value, &direct_character);
        WINDOW_INPUT routed;

        if (keyroute_event(&state, &event, 1, &routed) != KEYROUTE_WINDOW)
            continue;
        {
            UINT32 window_edit = keys_window_edit(
                routed.body.key.event.code, routed.body.key.event.value,
                routed.body.key.text, routed.body.key.event.text_length,
                &window_character);

            check(window_edit == direct_edit,
                  "a window's key makes the edit the direct path makes",
                  event.code, window_edit);
            check(window_character == direct_character,
                  "a window's key gives the character the direct path gives",
                  event.code, window_character);
            *edits += window_edit != EDIT_NONE;
            *inserts += window_edit == EDIT_INSERT;
        }
    }
}

static void check_window_edits(void)
{
    unsigned n, session, edits = 0, inserts = 0;

    check_window_edit_table();
    for (n = 0; n < SEEDS; n++) {
        seed_number = n;
        seed_random(n);
        for (session = 0; session < SESSIONS_PER_SEED; session++)
            check_window_edit_session(&edits, &inserts);
    }
    seed_number = NO_SEED;
    check(inserts > SEEDS * SESSIONS_PER_SEED &&
              edits > inserts + SEEDS * SESSIONS_PER_SEED,
          "the sessions reach characters and editing keys", inserts, edits);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    check_tables();
    check_sent_bits();
    check_text_of_every_code();
    check_sessions();
    check_window_edits();
    printf("%llu checks, %u failed, %u seeds of %u sessions\n", checks,
           failures, (unsigned)SEEDS, (unsigned)SESSIONS_PER_SEED);
    return failures != 0;
}
