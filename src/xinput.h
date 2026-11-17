/* XInput over USB: Xbox 360 wired pads, the Xbox 360 wireless receiver and
 * the many 2.4 GHz dongles and wired pads that imitate a 360 pad
 * (045e:028e and friends: ShanWan, Machenike, 8BitDo adapters in X-mode).
 * Protocol details as the Linux xpad driver has them. No USB here: the
 * transfers are in usb_pads.c, so all of this runs in the tests.
 *
 * A wired pad has one vendor interface, class FF / subclass 5D / protocol
 * 01, with an interrupt endpoint each way. The receiver (045e:0719) has
 * protocol 81 and four such interfaces, one per pad; its frames wrap the
 * pad's report with a 4-byte header. */
#ifndef ANYPAD_XINPUT_H
#define ANYPAD_XINPUT_H

#include "pad.h"

#include <stdint.h>

enum { XINPUT_NONE = 0, XINPUT_WIRED = 1, XINPUT_RECEIVER = 2 };

/* What an interface's class triple says. */
int xinput_iface_kind(unsigned cls, unsigned subclass, unsigned protocol);

/* A wired pad's input report (0x00 0x14, 20 bytes): 1 if read into st. */
int xinput_parse_wired(const uint8_t *r, int n, pad_state *st);

/* A receiver frame (29 bytes). Returns XINPUT_FRAME_INPUT with st filled,
 * XINPUT_FRAME_PRESENCE with *present set (a pad came or went), or
 * XINPUT_FRAME_OTHER. */
enum { XINPUT_FRAME_OTHER = 0, XINPUT_FRAME_INPUT = 1, XINPUT_FRAME_PRESENCE = 2 };
int xinput_parse_receiver(const uint8_t *r, int n, pad_state *st, int *present);

/* Messages to the pad; each returns its length. `player` is 1..4. */
int xinput_build_led(int kind, int player, uint8_t *out);
int xinput_build_rumble(int kind, uint8_t strong, uint8_t weak, uint8_t *out);
/* The receiver only: asks it whether a pad is there. */
int xinput_build_presence_query(uint8_t *out);

/* A readable name for a vendor/product, "" if unknown. */
const char *xinput_pad_name(unsigned vid, unsigned pid);

#endif
