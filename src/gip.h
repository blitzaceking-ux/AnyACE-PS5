/* The Xbox controller protocol (GIP) over USB: what is in the descriptors,
 * the messages, and the input report as a pad_state. No USB here, so it all
 * runs in the tests; the transfers are in usb_gip.c.
 *
 * Ported from FGG-XSense by FGG STORE (GPL-3.0),
 * https://github.com/FGGstore/FGG-XSense, and changed for AnyACE on
 * 2026-10-18: the parse fills AnyACE's pad_state, the Guide button is read,
 * and the descriptor walk is separate from the device so it can be tested.
 *
 * The PS5 enumerates a GIP gamepad and then leaves it alone: unconfigured,
 * unlit and silent. Every Xbox One / Series pad and the clones that speak
 * GIP present one interface of class FF, subclass 47, protocol D0, with an
 * interrupt endpoint each way. Xbox 360 pads (XInput, FF/5D/01) are a
 * different protocol and are not handled. */
#ifndef ANYPAD_GIP_H
#define ANYPAD_GIP_H

#include "pad.h"

#include <stdint.h>

/* Message types this code acts on. */
#define GIP_CMD_ACK              0x01
#define GIP_CMD_BATTERY          0x03
#define GIP_CMD_RUMBLE           0x09
#define GIP_CMD_SET_DEVICE_STATE 0x05
#define GIP_CMD_AUTHENTICATE     0x06
#define GIP_CMD_GUIDE_BUTTON     0x07
#define GIP_CMD_LED              0x0A
#define GIP_LL_INPUT_REPORT      0x20

/* Options byte: SYSTEM marks a system message; ACK means the pad wants the
 * message acknowledged and waits until it is. */
#define GIP_OPT_SYSTEM 0x20
#define GIP_OPT_ACK    0x10

/* Buttons in an input report's first two payload bytes. */
#define GIP_MENU   0x0004
#define GIP_VIEW   0x0008
#define GIP_A      0x0010
#define GIP_B      0x0020
#define GIP_X      0x0040
#define GIP_Y      0x0080
#define GIP_UP     0x0100
#define GIP_DOWN   0x0200
#define GIP_LEFT   0x0400
#define GIP_RIGHT  0x0800
#define GIP_LB     0x1000
#define GIP_RB     0x2000
#define GIP_LS     0x4000
#define GIP_RS     0x8000

#define GIP_MSG_MAX 64          /* the longest message built here */

/* The GIP interface and its interrupt endpoints, from a configuration
 * descriptor. */
typedef struct {
    int iface;                  /* bInterfaceNumber, -1: no GIP interface */
    int ep_in, ep_out;          /* endpoint addresses, -1: none */
    int mps_in;                 /* wMaxPacketSize of ep_in */
} gip_layout;

/* Walks a configuration descriptor. Returns 1 if a GIP interface with an
 * interrupt-IN endpoint was found; endpoints of other interfaces are ignored,
 * so a composite device's other functions cannot be taken for the pad. */
int gip_scan_layout(const unsigned char *cfg, int len, gip_layout *out);

/* The initialisation (and keepalive) messages, in order: power on,
 * authenticate (third-party pads switch themselves off without it), guide
 * LED on. Returns the message length, 0 past the end. `seq` is the sequence
 * number to put in the header. */
int gip_build_init(int step, unsigned char seq, unsigned char *out);
#define GIP_INIT_STEPS 3

/* The acknowledgement of `msg` (one with GIP_OPT_ACK set). Returns its length. */
int gip_build_ack(const unsigned char *msg, unsigned char *out);

/* An input report (GIP_LL_INPUT_REPORT, n >= 18) as a pad_state. Returns 0
 * for anything else. `guide` is kept from the last Guide message (below). */
int gip_parse_input(const unsigned char *msg, int n, int guide, pad_state *st);

/* A Guide button message (GIP_CMD_GUIDE_BUTTON): returns 1 pressed, 0
 * released, -1 if the message is not one. */
int gip_parse_guide(const unsigned char *msg, int n);

/* A battery message (GIP_CMD_BATTERY): returns 1 and the level (as a
 * percentage: empty, low, medium, full) and whether it is charging; 0 for a
 * pad with no battery (wired power); -1 if the message is not one. */
int gip_parse_battery(const unsigned char *msg, int n, int *pct, int *charging);

/* The rumble message: motors 0..255. Returns its length. */
int gip_build_rumble(unsigned char seq, uint8_t strong, uint8_t weak, unsigned char *out);

/* A readable name for a vendor/product, "" if unknown. */
const char *gip_pad_name(unsigned vid, unsigned pid);

#endif
