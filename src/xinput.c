#include "xinput.h"

#include <string.h>

/* Buttons in a wired report's bytes 2 and 3. */
#define X_UP     0x0001
#define X_DOWN   0x0002
#define X_LEFT   0x0004
#define X_RIGHT  0x0008
#define X_START  0x0010
#define X_BACK   0x0020
#define X_LS     0x0040
#define X_RS     0x0080
#define X_LB     0x0100
#define X_RB     0x0200
#define X_GUIDE  0x0400
#define X_A      0x1000
#define X_B      0x2000
#define X_X      0x4000
#define X_Y      0x8000

#define TRIGGER_DIGITAL 30          /* 0..255 past which L2/R2 count as pressed */

int xinput_iface_kind(unsigned cls, unsigned subclass, unsigned protocol)
{
    if (cls != 0xFF || subclass != 0x5D) return XINPUT_NONE;
    if (protocol == 0x81) return XINPUT_RECEIVER;
    if (protocol == 0x01) return XINPUT_WIRED;
    return XINPUT_NONE;
}

static int16_t s16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

/* Signed 16-bit with Y growing upwards, to 0..255 with Y growing downwards. */
static uint8_t axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

int xinput_parse_wired(const uint8_t *r, int n, pad_state *st)
{
    unsigned b;
    uint32_t out = 0;

    if (n < 14 || r[0] != 0x00) return 0;
    b = (unsigned)r[2] | (unsigned)r[3] << 8;
    pad_state_reset(st);
    if (b & X_A)     out |= PAD_CROSS;
    if (b & X_B)     out |= PAD_CIRCLE;
    if (b & X_X)     out |= PAD_SQUARE;
    if (b & X_Y)     out |= PAD_TRIANGLE;
    if (b & X_UP)    out |= PAD_UP;
    if (b & X_DOWN)  out |= PAD_DOWN;
    if (b & X_LEFT)  out |= PAD_LEFT;
    if (b & X_RIGHT) out |= PAD_RIGHT;
    if (b & X_LB)    out |= PAD_L1;
    if (b & X_RB)    out |= PAD_R1;
    if (b & X_LS)    out |= PAD_L3;
    if (b & X_RS)    out |= PAD_R3;
    if (b & X_START) out |= PAD_OPTIONS;
    if (b & X_BACK)  out |= PAD_CREATE;
    if (b & X_GUIDE) out |= PAD_PS;
    st->l2 = r[4];
    st->r2 = r[5];
    if (st->l2 > TRIGGER_DIGITAL) out |= PAD_L2;
    if (st->r2 > TRIGGER_DIGITAL) out |= PAD_R2;
    st->buttons = out;
    st->lx = axis(s16(r + 6));
    st->ly = axis_inv(s16(r + 8));
    st->rx = axis(s16(r + 10));
    st->ry = axis_inv(s16(r + 12));
    return 1;
}

/* Receiver frames: byte 0 bit 3 set announces presence (byte 1 bit 7: a
 * pad is there); otherwise byte 1 == 0x01 carries a pad report from byte 4
 * on, in the wired layout. */
int xinput_parse_receiver(const uint8_t *r, int n, pad_state *st, int *present)
{
    if (n < 2) return XINPUT_FRAME_OTHER;
    if (r[0] & 0x08) {
        *present = (r[1] & 0x80) != 0;
        return XINPUT_FRAME_PRESENCE;
    }
    if (r[1] == 0x01 && n >= 18 && xinput_parse_wired(r + 4, n - 4, st)) return XINPUT_FRAME_INPUT;
    return XINPUT_FRAME_OTHER;
}

int xinput_build_led(int kind, int player, uint8_t *out)
{
    uint8_t p = (uint8_t)(player < 1 ? 1 : player > 4 ? 4 : player);

    if (kind == XINPUT_RECEIVER) {
        memset(out, 0, 12);
        out[2] = 0x08;
        out[3] = (uint8_t)(0x40 + 2 + (p - 1));     /* 0x42..0x45: player lights, steady */
        return 12;
    }
    out[0] = 0x01;
    out[1] = 0x03;
    out[2] = (uint8_t)(2 + (p - 1));                /* 2..5: one light, steady */
    return 3;
}

int xinput_build_rumble(int kind, uint8_t strong, uint8_t weak, uint8_t *out)
{
    if (kind == XINPUT_RECEIVER) {
        memset(out, 0, 12);
        out[1] = 0x01;
        out[2] = 0x0F;
        out[3] = 0xC0;
        out[5] = strong;
        out[6] = weak;
        return 12;
    }
    memset(out, 0, 8);
    out[1] = 0x08;
    out[3] = strong;
    out[4] = weak;
    return 8;
}

int xinput_build_presence_query(uint8_t *out)
{
    out[0] = 0x08;
    out[1] = 0x00;
    out[2] = 0x0F;
    out[3] = 0xC0;
    return 4;
}

const char *xinput_pad_name(unsigned vid, unsigned pid)
{
    static const struct { unsigned vid, pid; const char *name; } k[] = {
        { 0x045E, 0x028E, "Xbox 360 Controller" },
        { 0x045E, 0x028F, "Xbox 360 Controller (wireless)" },
        { 0x045E, 0x0719, "Xbox 360 Wireless Receiver" },
        { 0x045E, 0x0291, "Xbox 360 Wireless Receiver" },
        { 0x1A34, 0xF517, "Machenike / ShanWan 2.4G dongle" },
        { 0x1A34, 0x0000, "ShanWan gamepad" },
        { 0x2345, 0x0000, "Machenike gamepad" },
        { 0x2DC8, 0x0000, "8BitDo (X-mode)" },
        { 0x0E6F, 0x0000, "PDP Xbox 360 controller" },
        { 0x0F0D, 0x0000, "Hori Xbox 360 controller" },
        { 0x24C6, 0x0000, "PowerA Xbox 360 controller" },
        { 0x1BAD, 0x0000, "Mad Catz Xbox 360 controller" },
        { 0x0738, 0x0000, "Mad Catz Xbox 360 controller" },
        { 0x146B, 0x0000, "BigBen Xbox 360 controller" },
        { 0x1532, 0x0000, "Razer Xbox 360 controller" },
    };
    size_t i;

    for (i = 0; i < sizeof k / sizeof k[0]; i++)
        if (k[i].vid == vid && (k[i].pid == pid || k[i].pid == 0)) return k[i].name;
    return "";
}
