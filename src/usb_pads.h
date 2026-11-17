/* Wired controllers on the console's USB ports, beside the Bluetooth pads:
 *
 *   GIP       Xbox One / Series pads and clones (gip.c)
 *   XInput    Xbox 360 wired pads, the 360 wireless receiver, and the
 *             2.4 GHz dongles and wired pads that imitate a 360 pad (xinput.c)
 *   HID       DualShock 4, DualShock 3, Switch Pro and NSO pads (their
 *             profiles, with the USB-only parts from usb_quirks.c), and any
 *             other HID game pad, read through its own report descriptor
 *             (generic.c)
 *
 * Scans the USB device nodes for them, brings each up, keeps it going and
 * turns its reports into pad_state through the same callbacks the Bluetooth
 * host uses. Slots start at `first_slot`, after the Bluetooth ones.
 *
 * The GIP part began as a port of FGG-XSense by FGG STORE (GPL-3.0),
 * https://github.com/FGGstore/FGG-XSense, changed for AnyACE in October
 * 2026. What its author learnt still holds for every kind here: a queued
 * transfer is left pending until it completes (cancelling and requeueing
 * reads as a broken host); refused and timed-out reads are ordinary; only a
 * device that is detached is gone. */
#ifndef ANYPAD_USB_PADS_H
#define ANYPAD_USB_PADS_H

#include "host.h"
#include "pad.h"

#define WIRED_MAX_PADS 4

enum { WIRED_ST_NONE, WIRED_ST_INIT, WIRED_ST_LIVE, WIRED_ST_SILENT };
enum { WIRED_GIP, WIRED_XINPUT, WIRED_X360W, WIRED_HID };

typedef struct {
    char node[32];              /* /dev/ugenX.Y */
    unsigned vid, pid;
    const char *name;           /* "" if unknown */
    const char *kind;           /* "gip", "xinput", "receiver", "hid" */
    int slot;                   /* its pad slot */
    int state;                  /* WIRED_ST_* */
    long reports;               /* input reports read so far */
} wired_pad_info;

/* `ev` are the same callbacks the Bluetooth host gets; `skip_node` is a
 * device node never to touch (the Bluetooth chip), NULL for none;
 * `map_dir` holds the mapping files for generic HID pads; `keepalive_s`
 * re-sends a GIP pad's wake-up sequence this often (0: never). */
void usb_pads_start(const host_events *ev, int first_slot, const char *skip_node,
                    const char *map_dir, int keepalive_s);
void usb_pads_poll(long now);
void usb_pads_stop(void);

/* Rumble and lights to the pad in `slot`. Returns 0 if there is no live
 * wired pad there. */
int  usb_pads_set_output(int slot, const pad_output *out);

/* The i-th attached wired pad, for the page. Returns 0 past the end. */
int  usb_pads_get(int i, wired_pad_info *info, pad_state *st);

#endif
