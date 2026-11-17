/* The Xbox USB protocol: the descriptor walk, the messages, and a report
 * as a pad_state. The report bytes follow what FGG-XSense verified on a
 * GameSir G7 (each button pressed alone and the raw bytes read). */
#include "../src/gip.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* A GIP pad's configuration descriptor: one interface FF/47/D0 with an
 * interrupt endpoint each way, as an Xbox One pad presents it. */
static const unsigned char k_xbox_cfg[] = {
    0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,       /* configuration */
    0x09, 0x04, 0x00, 0x00, 0x02, 0xFF, 0x47, 0xD0, 0x00,       /* interface 0: GIP */
    0x07, 0x05, 0x82, 0x03, 0x40, 0x00, 0x04,                   /* EP 0x82 interrupt IN, 64 */
    0x07, 0x05, 0x02, 0x03, 0x40, 0x00, 0x04,                   /* EP 0x02 interrupt OUT */
};

/* A composite device: an audio interface with its own interrupt endpoint
 * first, then the GIP one. The audio endpoint must not be taken. */
static const unsigned char k_composite_cfg[] = {
    0x09, 0x02, 0x39, 0x00, 0x02, 0x01, 0x00, 0xA0, 0xFA,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00,       /* interface 0: audio */
    0x07, 0x05, 0x81, 0x03, 0x10, 0x00, 0x08,                   /* EP 0x81 interrupt IN (audio) */
    0x09, 0x04, 0x01, 0x00, 0x02, 0xFF, 0x47, 0xD0, 0x00,       /* interface 1: GIP */
    0x07, 0x05, 0x83, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x03, 0x03, 0x40, 0x00, 0x04,
};

/* A DualSense's descriptor: HID, not GIP. */
static const unsigned char k_hid_cfg[] = {
    0x09, 0x02, 0x19, 0x00, 0x01, 0x01, 0x00, 0xA0, 0xFA,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x04,
};

static void test_layout(void)
{
    gip_layout l;

    printf("the GIP interface and its endpoints from a descriptor\n");
    CHECK(gip_scan_layout(k_xbox_cfg, sizeof k_xbox_cfg, &l));
    CHECK(l.iface == 0 && l.ep_in == 0x82 && l.ep_out == 0x02 && l.mps_in == 64);

    CHECK(gip_scan_layout(k_composite_cfg, sizeof k_composite_cfg, &l));
    CHECK(l.iface == 1 && l.ep_in == 0x83 && l.ep_out == 0x03);

    CHECK(!gip_scan_layout(k_hid_cfg, sizeof k_hid_cfg, &l));
    CHECK(!gip_scan_layout(k_xbox_cfg, 12, &l));                /* cut short */
    CHECK(!gip_scan_layout(k_xbox_cfg, 0, &l));
}

static void test_messages(void)
{
    unsigned char m[GIP_MSG_MAX], ack[GIP_MSG_MAX];
    static const unsigned char power[] = { 0x05, 0x20, 0x07, 0x01, 0x00 };
    static const unsigned char auth[]  = { 0x06, 0x20, 0x08, 0x02, 0x01, 0x00 };
    static const unsigned char led[]   = { 0x0A, 0x20, 0x09, 0x03, 0x00, 0x01, 0x14 };
    static const unsigned char wants_ack[] = { 0x04, 0x30, 0x2A, 0x05, 1, 2, 3, 4, 5 };

    printf("the initialisation messages and an acknowledgement\n");
    CHECK(gip_build_init(0, 7, m) == 5 && memcmp(m, power, 5) == 0);
    CHECK(gip_build_init(1, 8, m) == 6 && memcmp(m, auth, 6) == 0);
    CHECK(gip_build_init(2, 9, m) == 7 && memcmp(m, led, 7) == 0);
    CHECK(gip_build_init(3, 10, m) == 0);
    CHECK(gip_build_ack(wants_ack, ack) == 13);
    CHECK(ack[0] == 0x01 && ack[1] == 0x20 && ack[2] == 0x2A && ack[3] == 9);
    CHECK(ack[4] == 0 && ack[5] == 0x04 && ack[6] == 0x30 && ack[7] == 0x05);
}

static int report(unsigned char *r, unsigned buttons, unsigned lt, unsigned rt,
                  int lx, int ly, int rx, int ry)
{
    memset(r, 0, 18);
    r[0] = GIP_LL_INPUT_REPORT;
    r[1] = 0;
    r[2] = 1;
    r[3] = 14;
    r[4] = (unsigned char)buttons;
    r[5] = (unsigned char)(buttons >> 8);
    r[6] = (unsigned char)lt;  r[7] = (unsigned char)(lt >> 8);
    r[8] = (unsigned char)rt;  r[9] = (unsigned char)(rt >> 8);
    r[10] = (unsigned char)lx; r[11] = (unsigned char)(lx >> 8);
    r[12] = (unsigned char)ly; r[13] = (unsigned char)(ly >> 8);
    r[14] = (unsigned char)rx; r[15] = (unsigned char)(rx >> 8);
    r[16] = (unsigned char)ry; r[17] = (unsigned char)(ry >> 8);
    return 18;
}

static void test_input(void)
{
    unsigned char r[32];
    pad_state st;
    int n;

    printf("an input report as a pad_state\n");
    n = report(r, GIP_A | GIP_LB | GIP_MENU | GIP_VIEW | GIP_UP, 0, 1023, 0, 0, 0, 0);
    CHECK(gip_parse_input(r, n, 0, &st));
    CHECK(st.buttons == (PAD_CROSS | PAD_L1 | PAD_OPTIONS | PAD_CREATE | PAD_UP | PAD_R2));
    CHECK(st.lx == 128 && st.ly == 127 && st.rx == 128 && st.ry == 127);  /* centred */
    CHECK(st.l2 == 0 && st.r2 == 255);
    CHECK(!st.has_battery);

    n = report(r, GIP_B | GIP_X | GIP_Y | GIP_RB | GIP_LS | GIP_RS | GIP_DOWN | GIP_LEFT | GIP_RIGHT,
               600, 100, 32767, 32767, -32768, -32768);
    CHECK(gip_parse_input(r, n, 1, &st));
    CHECK(st.buttons == (PAD_CIRCLE | PAD_SQUARE | PAD_TRIANGLE | PAD_R1 | PAD_L3 | PAD_R3 |
                         PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_L2 | PAD_PS));
    CHECK(st.lx == 255 && st.ly == 0);          /* right, up */
    CHECK(st.rx == 0 && st.ry == 255);          /* left, down */
    CHECK(st.l2 == 150 && st.r2 == 25);

    /* Trigger just past the digital threshold, and just under. */
    n = report(r, 0, 129, 128, 0, 0, 0, 0);
    CHECK(gip_parse_input(r, n, 0, &st) && st.buttons == PAD_L2);

    /* Not an input report, or too short. */
    r[0] = GIP_CMD_GUIDE_BUTTON;
    CHECK(!gip_parse_input(r, n, 0, &st));
    r[0] = GIP_LL_INPUT_REPORT;
    CHECK(!gip_parse_input(r, 17, 0, &st));
}

static void test_guide(void)
{
    static const unsigned char down[] = { 0x07, 0x20, 0x03, 0x02, 0x01, 0x5B };
    static const unsigned char up[]   = { 0x07, 0x20, 0x04, 0x02, 0x00, 0x5B };
    static const unsigned char other[] = { 0x20, 0x00, 0x04, 0x02, 0x00, 0x5B };

    printf("the Guide button\n");
    CHECK(gip_parse_guide(down, sizeof down) == 1);
    CHECK(gip_parse_guide(up, sizeof up) == 0);
    CHECK(gip_parse_guide(other, sizeof other) == -1);
    CHECK(gip_parse_guide(down, 4) == -1);
}

static void test_battery_and_rumble(void)
{
    static const unsigned char full[] = { 0x03, 0x20, 0x01, 0x01, 0x07 };    /* kind 1, level 3 */
    static const unsigned char low_charging[] = { 0x03, 0x20, 0x01, 0x01, 0x19 };
    static const unsigned char wired[] = { 0x03, 0x20, 0x01, 0x01, 0x00 };
    unsigned char m[GIP_MSG_MAX];
    int pct = -1, ch = -1;

    printf("battery messages and the rumble message\n");
    CHECK(gip_parse_battery(full, 5, &pct, &ch) == 1 && pct == 100 && !ch);
    CHECK(gip_parse_battery(low_charging, 5, &pct, &ch) == 1 && pct == 35 && ch);
    CHECK(gip_parse_battery(wired, 5, &pct, &ch) == 0);
    CHECK(gip_parse_battery(full, 4, &pct, &ch) == -1);
    m[0] = 0x20;
    CHECK(gip_parse_battery(m, 5, &pct, &ch) == -1);

    CHECK(gip_build_rumble(0x11, 255, 128, m) == 13);
    CHECK(m[0] == 0x09 && m[1] == 0x00 && m[2] == 0x11 && m[3] == 9);
    CHECK(m[4] == 0 && m[5] == 0x0F && m[6] == 0 && m[7] == 0 && m[8] == 100 && m[9] == 50 && m[10] == 0xFF);
    CHECK(gip_build_rumble(0, 0, 0, m) == 13 && m[8] == 0 && m[9] == 0);
}

static void test_names(void)
{
    printf("pad names\n");
    CHECK(strcmp(gip_pad_name(0x045E, 0x0B12), "Xbox Series X|S Controller") == 0);
    CHECK(strcmp(gip_pad_name(0x3537, 0x1010), "GameSir G7") == 0);
    CHECK(strcmp(gip_pad_name(0x3537, 0x9999), "GameSir Xbox controller") == 0);
    CHECK(strcmp(gip_pad_name(0x045E, 0x9999), "Xbox controller") == 0);
    CHECK(strcmp(gip_pad_name(0x1234, 0x0001), "") == 0);
}

int main(void)
{
    test_layout();
    test_messages();
    test_input();
    test_guide();
    test_battery_and_rumble();
    test_names();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all GIP checks passed\n");
    return 0;
}
