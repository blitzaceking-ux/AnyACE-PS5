/* The USB-only parts of HID pads: known models, wake requests and reports,
 * output reports in their USB shape. */
#include "../src/usb_quirks.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main(void)
{
    usb_ctl ctl;
    uint8_t d[64];
    pad_output o = { 200, 100, 1, 2, 3, 1 };

    printf("known models\n");
    CHECK(usb_quirks_model(0x054C, 0x09CC) == USBPAD_DS4);
    CHECK(usb_quirks_model(0x054C, 0x0268) == USBPAD_DS3);
    CHECK(usb_quirks_model(0x057E, 0x2009) == USBPAD_SWITCH);
    CHECK(usb_quirks_model(0x0079, 0x0006) == USBPAD_HID);
    CHECK(strcmp(usb_quirks_name(0x054C, 0x0268), "DualShock 3 (USB)") == 0);
    CHECK(strcmp(usb_quirks_name(0x0079, 0x0006), "") == 0);

    printf("waking a DualShock 3\n");
    CHECK(usb_quirks_wake_control(USBPAD_DS3, &ctl, d, sizeof d) == 4);
    CHECK(ctl.request_type == 0x21 && ctl.request == 0x09 && ctl.value == 0x03F4 && ctl.index == 0);
    CHECK(d[0] == 0x42 && d[1] == 0x0C && d[2] == 0 && d[3] == 0);
    CHECK(usb_quirks_wake_control(USBPAD_DS4, &ctl, d, sizeof d) == 0);
    CHECK(usb_quirks_wake_control(USBPAD_SWITCH, &ctl, d, sizeof d) == 0);

    printf("the Switch USB handshake\n");
    CHECK(usb_quirks_start_report(USBPAD_SWITCH, 0, d, sizeof d) == 2 && d[0] == 0x80 && d[1] == 0x02);
    CHECK(usb_quirks_start_report(USBPAD_SWITCH, 1, d, sizeof d) == 2 && d[0] == 0x80 && d[1] == 0x04);
    CHECK(usb_quirks_start_report(USBPAD_SWITCH, 2, d, sizeof d) == 0);
    CHECK(usb_quirks_start_report(USBPAD_DS4, 0, d, sizeof d) == 0);

    printf("the DualShock 4's USB output report\n");
    CHECK(usb_quirks_output(USBPAD_DS4, &o, d, sizeof d) == 32);
    CHECK(d[0] == 0x05 && d[1] == 0x07 && d[4] == 100 && d[5] == 200 && d[6] == 1 && d[7] == 2 && d[8] == 3);
    CHECK(usb_quirks_output(USBPAD_SWITCH, &o, d, sizeof d) == 0);     /* the profile's is fine */
    CHECK(usb_quirks_output(USBPAD_DS3, &o, d, sizeof d) == 0);

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all USB quirk checks passed\n");
    return 0;
}
