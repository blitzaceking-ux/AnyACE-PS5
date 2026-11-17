/* XInput over USB: the interface kinds, wired and receiver reports, and the
 * messages to the pad, as the Linux xpad driver has them. */
#include "../src/xinput.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int wired(uint8_t *r, unsigned buttons, uint8_t lt, uint8_t rt, int lx, int ly, int rx, int ry)
{
    memset(r, 0, 20);
    r[0] = 0x00;
    r[1] = 0x14;
    r[2] = (uint8_t)buttons;
    r[3] = (uint8_t)(buttons >> 8);
    r[4] = lt;
    r[5] = rt;
    r[6] = (uint8_t)lx;  r[7] = (uint8_t)(lx >> 8);
    r[8] = (uint8_t)ly;  r[9] = (uint8_t)(ly >> 8);
    r[10] = (uint8_t)rx; r[11] = (uint8_t)(rx >> 8);
    r[12] = (uint8_t)ry; r[13] = (uint8_t)(ry >> 8);
    return 20;
}

static void test_kinds(void)
{
    printf("interface kinds\n");
    CHECK(xinput_iface_kind(0xFF, 0x5D, 0x01) == XINPUT_WIRED);
    CHECK(xinput_iface_kind(0xFF, 0x5D, 0x81) == XINPUT_RECEIVER);
    CHECK(xinput_iface_kind(0xFF, 0x47, 0xD0) == XINPUT_NONE);      /* GIP */
    CHECK(xinput_iface_kind(0x03, 0x00, 0x00) == XINPUT_NONE);      /* HID */
    CHECK(xinput_iface_kind(0xFF, 0x5D, 0x02) == XINPUT_NONE);      /* the headset */
}

static void test_wired(void)
{
    uint8_t r[32];
    pad_state st;
    int n;

    printf("a wired pad's report\n");
    n = wired(r, 0x1000 | 0x0100 | 0x0010 | 0x0001, 0, 255, 0, 0, 0, 0);      /* A, LB, Start, Up */
    CHECK(xinput_parse_wired(r, n, &st));
    CHECK(st.buttons == (PAD_CROSS | PAD_L1 | PAD_OPTIONS | PAD_UP | PAD_R2));
    CHECK(st.l2 == 0 && st.r2 == 255);
    CHECK(st.lx == 128 && st.ly == 127 && st.rx == 128 && st.ry == 127);

    n = wired(r, 0x2000 | 0x4000 | 0x8000 | 0x0200 | 0x0040 | 0x0080 | 0x0400 | 0x0020 | 0x0002 | 0x0004 | 0x0008,
              100, 10, 32767, 32767, -32768, -32768);
    CHECK(xinput_parse_wired(r, n, &st));
    CHECK(st.buttons == (PAD_CIRCLE | PAD_SQUARE | PAD_TRIANGLE | PAD_R1 | PAD_L3 | PAD_R3 | PAD_PS |
                         PAD_CREATE | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L2));
    CHECK(st.l2 == 100 && st.r2 == 10);
    CHECK(st.lx == 255 && st.ly == 0 && st.rx == 0 && st.ry == 255);

    r[0] = 0x01;                                    /* not an input report */
    CHECK(!xinput_parse_wired(r, n, &st));
    r[0] = 0x00;
    CHECK(!xinput_parse_wired(r, 13, &st));         /* too short */
}

static void test_receiver(void)
{
    uint8_t f[29], inner[20];
    pad_state st;
    int present = -1;

    printf("the wireless receiver's frames\n");
    memset(f, 0, sizeof f);
    f[0] = 0x08; f[1] = 0x80;
    CHECK(xinput_parse_receiver(f, 29, &st, &present) == XINPUT_FRAME_PRESENCE && present == 1);
    f[1] = 0x00;
    CHECK(xinput_parse_receiver(f, 29, &st, &present) == XINPUT_FRAME_PRESENCE && present == 0);

    memset(f, 0, sizeof f);
    f[0] = 0x00; f[1] = 0x01; f[2] = 0x00; f[3] = 0xF0;
    wired(inner, 0x1000, 0, 0, 0, 0, 0, 0);
    inner[1] = 0x13;                                /* the receiver says 0x13 here */
    memcpy(f + 4, inner, 20);
    CHECK(xinput_parse_receiver(f, 29, &st, &present) == XINPUT_FRAME_INPUT);
    CHECK(st.buttons == PAD_CROSS);

    f[1] = 0x0F;                                    /* battery or whatever else */
    CHECK(xinput_parse_receiver(f, 29, &st, &present) == XINPUT_FRAME_OTHER);
    CHECK(xinput_parse_receiver(f, 1, &st, &present) == XINPUT_FRAME_OTHER);
}

static void test_messages(void)
{
    uint8_t m[16];

    printf("lights, rumble and the presence query\n");
    CHECK(xinput_build_led(XINPUT_WIRED, 1, m) == 3 && m[0] == 0x01 && m[1] == 0x03 && m[2] == 0x02);
    CHECK(xinput_build_led(XINPUT_WIRED, 4, m) == 3 && m[2] == 0x05);
    CHECK(xinput_build_led(XINPUT_WIRED, 9, m) == 3 && m[2] == 0x05);       /* clamped */
    CHECK(xinput_build_led(XINPUT_RECEIVER, 2, m) == 12 && m[2] == 0x08 && m[3] == 0x43);
    CHECK(xinput_build_rumble(XINPUT_WIRED, 200, 100, m) == 8);
    CHECK(m[0] == 0x00 && m[1] == 0x08 && m[2] == 0x00 && m[3] == 200 && m[4] == 100);
    CHECK(xinput_build_rumble(XINPUT_RECEIVER, 200, 100, m) == 12);
    CHECK(m[1] == 0x01 && m[2] == 0x0F && m[3] == 0xC0 && m[5] == 200 && m[6] == 100);
    CHECK(xinput_build_presence_query(m) == 4 && m[0] == 0x08 && m[3] == 0xC0);
}

static void test_names(void)
{
    printf("pad names\n");
    CHECK(strcmp(xinput_pad_name(0x045E, 0x028E), "Xbox 360 Controller") == 0);
    CHECK(strcmp(xinput_pad_name(0x1A34, 0xF517), "Machenike / ShanWan 2.4G dongle") == 0);
    CHECK(strcmp(xinput_pad_name(0x1A34, 0x1234), "ShanWan gamepad") == 0);
    CHECK(strcmp(xinput_pad_name(0x1234, 0x0001), "") == 0);
}

int main(void)
{
    test_kinds();
    test_wired();
    test_receiver();
    test_messages();
    test_names();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all XInput checks passed\n");
    return 0;
}
