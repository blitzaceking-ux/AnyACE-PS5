/* What HID game pads need over USB that they do not need over Bluetooth:
 * which models are known, the control request or output reports that wake
 * them, and output reports in their USB shape. The parsing of their input
 * is the same as over Bluetooth (profiles.c). No USB here. */
#ifndef ANYPAD_USB_QUIRKS_H
#define ANYPAD_USB_QUIRKS_H

#include "pad.h"

#include <stdint.h>

enum {
    USBPAD_UNKNOWN = 0,
    USBPAD_DS4,                 /* DualShock 4: reports as over Bluetooth's 0x01, output 0x05 */
    USBPAD_DS3,                 /* DualShock 3: a feature report wakes it */
    USBPAD_SWITCH,              /* Switch Pro and NSO pads: 0x80 handshakes, then the Bluetooth sequence */
    USBPAD_HID,                 /* anything else that is a HID game pad: by its report descriptor */
};

/* The model for a vendor/product, USBPAD_HID when not known specially. */
int usb_quirks_model(unsigned vid, unsigned pid);
const char *usb_quirks_name(unsigned vid, unsigned pid);

/* A control request (SET_REPORT and the like). */
typedef struct {
    uint8_t  request_type, request;
    uint16_t value, index;
} usb_ctl;

/* The one control request that wakes the model, with its data; returns the
 * data length, 0 if the model needs none. */
int usb_quirks_wake_control(int model, usb_ctl *ctl, uint8_t *data, int max);

/* Output reports to send on the interrupt OUT endpoint when the pad is
 * found, one per step; 0 past the end. */
int usb_quirks_start_report(int model, int step, uint8_t *out, int max);

/* The output report (rumble, lights) in the model's USB shape; 0 means the
 * profile's own (Bluetooth) output report is right for USB too. */
int usb_quirks_output(int model, const pad_output *o, uint8_t *out, int max);

#endif
