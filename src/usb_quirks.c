#include "usb_quirks.h"

#include <string.h>

static const struct { unsigned vid, pid; int model; const char *name; } k_models[] = {
    { 0x054C, 0x05C4, USBPAD_DS4, "DualShock 4 (USB)" },
    { 0x054C, 0x09CC, USBPAD_DS4, "DualShock 4 v2 (USB)" },
    { 0x054C, 0x0BA0, USBPAD_DS4, "DualShock 4 USB adapter" },
    { 0x054C, 0x0268, USBPAD_DS3, "DualShock 3 (USB)" },
    { 0x057E, 0x2009, USBPAD_SWITCH, "Switch Pro Controller (USB)" },
    { 0x057E, 0x2017, USBPAD_SWITCH, "NSO SNES controller (USB)" },
    { 0x057E, 0x2019, USBPAD_SWITCH, "NSO N64 controller (USB)" },
    { 0x057E, 0x201A, USBPAD_SWITCH, "NSO Mega Drive controller (USB)" },
    { 0x2DC8, 0x3012, USBPAD_SWITCH, "8BitDo adapter (Switch mode)" },
};

int usb_quirks_model(unsigned vid, unsigned pid)
{
    size_t i;

    for (i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid) return k_models[i].model;
    return USBPAD_HID;
}

const char *usb_quirks_name(unsigned vid, unsigned pid)
{
    size_t i;

    for (i = 0; i < sizeof k_models / sizeof k_models[0]; i++)
        if (k_models[i].vid == vid && k_models[i].pid == pid) return k_models[i].name;
    return "";
}

int usb_quirks_wake_control(int model, usb_ctl *ctl, uint8_t *data, int max)
{
    if (model == USBPAD_DS3 && max >= 4) {
        /* SET_REPORT of feature report 0xF4: "start sending input". */
        ctl->request_type = 0x21;       /* host to device, class, interface */
        ctl->request = 0x09;
        ctl->value = 0x03F4;            /* feature report, id 0xF4 */
        ctl->index = 0;
        data[0] = 0x42; data[1] = 0x0C; data[2] = 0x00; data[3] = 0x00;
        return 4;
    }
    return 0;
}

int usb_quirks_start_report(int model, int step, uint8_t *out, int max)
{
    if (model == USBPAD_SWITCH && max >= 2) {
        /* The USB handshake the Switch does: then the pad takes the same
         * subcommands as over Bluetooth, and sends the same reports. */
        switch (step) {
        case 0: out[0] = 0x80; out[1] = 0x02; return 2;       /* handshake */
        case 1: out[0] = 0x80; out[1] = 0x04; return 2;       /* USB only, no timeout */
        default: return 0;
        }
    }
    return 0;
}

int usb_quirks_output(int model, const pad_output *o, uint8_t *out, int max)
{
    if (model == USBPAD_DS4 && max >= 32) {
        /* Report 0x05: [1] flags (rumble and light bar valid) [4] weak
         * [5] strong [6..8] colour, the rest zero. */
        memset(out, 0, 32);
        out[0] = 0x05;
        out[1] = 0x07;
        out[4] = o->weak;
        out[5] = o->strong;
        out[6] = o->r;
        out[7] = o->g;
        out[8] = o->b;
        return 32;
    }
    return 0;
}
