#include "gip.h"

#include <string.h>

/* USB descriptor constants, so this file needs no system USB headers. */
#define DESC_INTERFACE 0x04
#define DESC_ENDPOINT  0x05
#define EP_DIR_IN      0x80
#define EP_TYPE_MASK   0x03
#define EP_INTERRUPT   0x03

#define GIP_IFACE_CLASS    0xFF
#define GIP_IFACE_SUBCLASS 0x47
#define GIP_IFACE_PROTOCOL 0xD0

/* Guide LED message payload: which LED, the pattern, and a brightness. */
#define GIP_LED_GUIDE    0x00
#define GIP_LED_GUIDE_ON 0x01
#define GIP_LED_LEVEL    0x14

/* A trigger past this (0..1023) also sets the digital L2/R2 bit. */
#define TRIGGER_DIGITAL 128

int gip_scan_layout(const unsigned char *cfg, int len, gip_layout *out)
{
    int i = 0, in_gip = 0;

    out->iface = out->ep_in = out->ep_out = -1;
    out->mps_in = 0;
    while (i + 2 <= len) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0 || i + blen > len) break;
        if (btype == DESC_INTERFACE && blen >= 9) {
            in_gip = cfg[i + 5] == GIP_IFACE_CLASS && cfg[i + 6] == GIP_IFACE_SUBCLASS &&
                     cfg[i + 7] == GIP_IFACE_PROTOCOL;
            if (in_gip && out->iface < 0) out->iface = cfg[i + 2];
            else in_gip = in_gip && out->iface == cfg[i + 2];
        } else if (btype == DESC_ENDPOINT && blen >= 7 && in_gip) {
            int addr = cfg[i + 2], attr = cfg[i + 3];
            int mps = cfg[i + 4] | cfg[i + 5] << 8;
            if ((attr & EP_TYPE_MASK) == EP_INTERRUPT) {
                if ((addr & EP_DIR_IN) && out->ep_in < 0) {
                    out->ep_in = addr;
                    out->mps_in = mps;
                } else if (!(addr & EP_DIR_IN) && out->ep_out < 0) {
                    out->ep_out = addr;
                }
            }
        }
        i += blen;
    }
    return out->iface >= 0 && out->ep_in >= 0;
}

static int build(unsigned char *out, unsigned char cmd, unsigned char seq,
                 const unsigned char *payload, int plen)
{
    out[0] = cmd;
    out[1] = GIP_OPT_SYSTEM;
    out[2] = seq;
    out[3] = (unsigned char)plen;
    memcpy(out + 4, payload, (size_t)plen);
    return 4 + plen;
}

int gip_build_init(int step, unsigned char seq, unsigned char *out)
{
    static const unsigned char power_on[] = { 0x00 };
    static const unsigned char auth[]     = { 0x01, 0x00 };
    static const unsigned char led_on[]   = { GIP_LED_GUIDE, GIP_LED_GUIDE_ON, GIP_LED_LEVEL };

    switch (step) {
    case 0: return build(out, GIP_CMD_SET_DEVICE_STATE, seq, power_on, sizeof power_on);
    case 1: return build(out, GIP_CMD_AUTHENTICATE, seq, auth, sizeof auth);
    case 2: return build(out, GIP_CMD_LED, seq, led_on, sizeof led_on);
    default: return 0;
    }
}

/* The payload carries the command, options and length of the message being
 * answered; the header echoes its sequence number (as Linux xpad does in
 * xpadone_ack_mode_report). */
int gip_build_ack(const unsigned char *msg, unsigned char *out)
{
    unsigned char body[9];

    memset(body, 0, sizeof body);
    body[1] = msg[0];
    body[2] = msg[1];
    body[3] = msg[3];
    return build(out, GIP_CMD_ACK, msg[2], body, sizeof body);
}

static int16_t s16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/* GIP sticks are signed 16-bit with Y growing upwards; a pad_state has
 * unsigned 8-bit axes with Y growing downwards. */
static uint8_t axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

int gip_parse_input(const unsigned char *msg, int n, int guide, pad_state *st)
{
    const unsigned char *d = msg + 4;
    unsigned m, lt, rt;
    uint32_t b = 0;

    if (n < 18 || msg[0] != GIP_LL_INPUT_REPORT) return 0;
    m = (unsigned)d[0] | (unsigned)d[1] << 8;
    lt = (unsigned)d[2] | (unsigned)d[3] << 8;
    rt = (unsigned)d[4] | (unsigned)d[5] << 8;

    pad_state_reset(st);
    if (m & GIP_A)     b |= PAD_CROSS;
    if (m & GIP_B)     b |= PAD_CIRCLE;
    if (m & GIP_X)     b |= PAD_SQUARE;
    if (m & GIP_Y)     b |= PAD_TRIANGLE;
    if (m & GIP_UP)    b |= PAD_UP;
    if (m & GIP_DOWN)  b |= PAD_DOWN;
    if (m & GIP_LEFT)  b |= PAD_LEFT;
    if (m & GIP_RIGHT) b |= PAD_RIGHT;
    if (m & GIP_LB)    b |= PAD_L1;
    if (m & GIP_RB)    b |= PAD_R1;
    if (m & GIP_LS)    b |= PAD_L3;
    if (m & GIP_RS)    b |= PAD_R3;
    if (m & GIP_MENU)  b |= PAD_OPTIONS;
    if (m & GIP_VIEW)  b |= PAD_CREATE;
    if (lt > TRIGGER_DIGITAL) b |= PAD_L2;
    if (rt > TRIGGER_DIGITAL) b |= PAD_R2;
    if (guide) b |= PAD_PS;
    st->buttons = b;

    st->lx = axis(s16(d + 6));
    st->ly = axis_inv(s16(d + 8));
    st->rx = axis(s16(d + 10));
    st->ry = axis_inv(s16(d + 12));
    st->l2 = (uint8_t)(lt > 1023 ? 255 : lt >> 2);
    st->r2 = (uint8_t)(rt > 1023 ? 255 : rt >> 2);
    return 1;
}

/* Battery status, as Linux xpad reads it: bits 0-1 the level, bits 2-3 the
 * kind of battery (0: none), bit 4 charging. */
int gip_parse_battery(const unsigned char *msg, int n, int *pct, int *charging)
{
    static const int levels[4] = { 5, 35, 70, 100 };
    unsigned b;

    if (n < 5 || msg[0] != GIP_CMD_BATTERY) return -1;
    b = msg[4];
    if (((b >> 2) & 3) == 0) return 0;
    *pct = levels[b & 3];
    *charging = (b & 0x10) != 0;
    return 1;
}

/* As Linux xpad sends it: no trigger motors, both main motors as 0..100,
 * for as long as the pad allows (0xFF), then stopped by a message of zeros. */
int gip_build_rumble(unsigned char seq, uint8_t strong, uint8_t weak, unsigned char *out)
{
    unsigned char p[9] = { 0x00, 0x0F, 0, 0, 0, 0, 0xFF, 0x00, 0x00 };

    p[4] = (unsigned char)(strong * 100 / 255);
    p[5] = (unsigned char)(weak * 100 / 255);
    out[0] = GIP_CMD_RUMBLE;
    out[1] = 0x00;
    out[2] = seq;
    out[3] = (unsigned char)sizeof p;
    memcpy(out + 4, p, sizeof p);
    return 4 + (int)sizeof p;
}

int gip_parse_guide(const unsigned char *msg, int n)
{
    if (n < 5 || msg[0] != GIP_CMD_GUIDE_BUTTON) return -1;
    return msg[4] & 1;
}

const char *gip_pad_name(unsigned vid, unsigned pid)
{
    static const struct { unsigned vid, pid; const char *name; } k[] = {
        { 0x045E, 0x02D1, "Xbox One Controller" },
        { 0x045E, 0x02DD, "Xbox One Controller (2015)" },
        { 0x045E, 0x02E3, "Xbox One Elite Controller" },
        { 0x045E, 0x02EA, "Xbox One S Controller" },
        { 0x045E, 0x0B00, "Xbox Elite Series 2" },
        { 0x045E, 0x0B12, "Xbox Series X|S Controller" },
        { 0x045E, 0x0B0A, "Xbox Adaptive Controller" },
        { 0x3537, 0x1010, "GameSir G7" },
        { 0x3537, 0x1022, "GameSir G7 Pro" },
        { 0x0E6F, 0x0000, "PDP Xbox controller" },
        { 0x0F0D, 0x0000, "Hori Xbox controller" },
        { 0x24C6, 0x0000, "PowerA Xbox controller" },
        { 0x2DC8, 0x0000, "8BitDo Xbox controller" },
        { 0x1532, 0x0000, "Razer Xbox controller" },
        { 0x3537, 0x0000, "GameSir Xbox controller" },
    };
    size_t i;

    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].vid == vid && (k[i].pid == pid || k[i].pid == 0)) return k[i].name;
    if (vid == 0x045E) return "Xbox controller";
    return "";
}
