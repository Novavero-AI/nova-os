/* How a pointer's driver gathers its device's events into pointer
   reports (docs/spec/input.md K2, input_protocol.h): pure, so that the
   driver runs it on the events it takes from the device and
   tools/check-pointer.c on events it makes up. */

#ifndef NOVA_POINTER_H
#define NOVA_POINTER_H

#include "input_protocol.h"

/* An EV_BITS bitmap as VIRTIO 1.4 5.8.4 gives it: code n in bit n % 8
   of byte n / 8, in the 128 bytes of the configuration's union. */
#define POINTER_BITMAP_BYTES 128

/* evdev's types and codes a pointer's report takes besides the
   protocol's, as the Linux input header numbers them. */
#define POINTER_EV_REL 0x02
#define POINTER_EV_ABS 0x03
#define POINTER_SYN_DROPPED 3
#define POINTER_REL_X 0x00
#define POINTER_REL_Y 0x01
#define POINTER_REL_WHEEL 0x08
#define POINTER_ABS_X 0x00
#define POINTER_ABS_Y 0x01

/* A pointer: the codes it declared, the absolute axes it has with a
   range, the buttons its client knows held, whether it is dropping
   events after a SYN_DROPPED, and the report so far. */
typedef struct {
    UINT8 keys[POINTER_BITMAP_BYTES]; /* EV_KEY */
    UINT8 rel[POINTER_BITMAP_BYTES];  /* EV_REL */
    UINT32 absolute;                  /* INPUT_AXIS_X and _Y */
    UINT8 held[POINTER_BITMAP_BYTES];
    UINT32 dropping;
    INPUT_POINTER report;
} POINTER;

/* Whether bitmap, an EV_BITS bitmap, has code. */
int pointer_has(const UINT8 *bitmap, UINT32 code);

/* Start pointer with no button held, nothing gathered, and the codes
   and axes its keys, rel and absolute already hold. */
void pointer_start(POINTER *pointer);

/* Take one event of the device's, its value as the device wrote it.
   Returns 1 with a report in *out, every field set but time, when the
   event ends one: a SYN_REPORT after something taken, or a change of a
   button the report already changes, or one past INPUT_REPORT_BUTTONS,
   which then starts the next report; else 0. An absolute axis it does
   not have, a code it did not declare, a button's repeat, a release of
   a button not held and a press of one held are not taken. A
   SYN_DROPPED drops what was gathered and every event up to and
   including the next SYN_REPORT. The buttons held are then those the
   client knows, which a dropped change leaves unlike the device's:
   evdev has a client read the device's state again (evdev,
   SYN_DROPPED), which VIRTIO 1.4 gives no way to. */
int pointer_take(POINTER *pointer, UINT16 type, UINT16 code, UINT32 value,
                 INPUT_POINTER *out);

#endif
