#include "remap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const remap_names[REMAP_BUTTONS] = {
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3",
    "up", "down", "left", "right", "options", "create", "ps", "touchpad",
};

const uint32_t remap_bits[REMAP_BUTTONS] = {
    PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE, PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_L3, PAD_R3,
    PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_OPTIONS, PAD_CREATE, PAD_PS, PAD_TOUCHPAD,
};

#define B_L2 6
#define B_R2 7

void remap_identity(remap_t *r)
{
    int i;

    memset(r, 0, sizeof *r);
    for (i = 0; i < REMAP_BUTTONS; i++) r->to[i] = (uint8_t)i;
}

int remap_is_identity(const remap_t *r)
{
    int i;

    for (i = 0; i < REMAP_BUTTONS; i++) if (r->to[i] != i) return 0;
    return !r->swap_sticks && !r->invert_lx && !r->invert_ly && !r->invert_rx && !r->invert_ry;
}

/* The analogue value a source button contributes when it lands on a trigger. */
static uint8_t trigger_value(const pad_state *st, int from)
{
    if (from == B_L2) return st->l2;
    if (from == B_R2) return st->r2;
    return st->buttons & remap_bits[from] ? 255 : 0;
}

void remap_apply(const remap_t *r, pad_state *st)
{
    uint32_t in = st->buttons, out = 0;
    uint8_t l2 = 0, r2 = 0, t;
    int i;

    if (remap_is_identity(r)) return;
    for (i = 0; i < REMAP_BUTTONS; i++) {
        int to = r->to[i];
        if (to >= REMAP_BUTTONS) continue;
        if (in & remap_bits[i]) out |= remap_bits[to];
        /* A trigger's analogue travel follows whatever lands on it. */
        if (to == B_L2 && (t = trigger_value(st, i)) > l2) l2 = t;
        if (to == B_R2 && (t = trigger_value(st, i)) > r2) r2 = t;
    }
    st->buttons = out;
    st->l2 = l2;
    st->r2 = r2;

    if (r->swap_sticks) {
        t = st->lx; st->lx = st->rx; st->rx = t;
        t = st->ly; st->ly = st->ry; st->ry = t;
    }
    if (r->invert_lx) st->lx = (uint8_t)(255 - st->lx);
    if (r->invert_ly) st->ly = (uint8_t)(255 - st->ly);
    if (r->invert_rx) st->rx = (uint8_t)(255 - st->rx);
    if (r->invert_ry) st->ry = (uint8_t)(255 - st->ry);
}

static int button_index(const char *name, size_t n)
{
    int i;

    if (n == 4 && strncmp(name, "none", 4) == 0) return REMAP_NONE;
    for (i = 0; i < REMAP_BUTTONS; i++)
        if (strlen(remap_names[i]) == n && strncmp(name, remap_names[i], n) == 0) return i;
    return -1;
}

static const char *trim(const char *s, const char *end, size_t *len)
{
    while (s < end && isspace((unsigned char)*s)) s++;
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *len = (size_t)(end - s);
    return s;
}

int remap_parse(remap_t *r, const char *text)
{
    const char *p = text, *nl;
    int used = 0, written = 0;

    remap_identity(r);
    while (p && *p) {
        const char *eq, *k, *v;
        size_t kn, vn;
        nl = strchr(p, '\n');
        if (!nl) nl = p + strlen(p);
        eq = memchr(p, '=', (size_t)(nl - p));
        k = trim(p, eq ? eq : nl, &kn);
        if (kn && k[0] != '#') {
            written = 1;
            if (eq) {
                int from = button_index(k, kn), to;
                v = trim(eq + 1, nl, &vn);
                if (from >= 0 && from < REMAP_BUTTONS && (to = button_index(v, vn)) >= 0) {
                    r->to[from] = (uint8_t)to;
                    used = 1;
                } else if (vn == 1 && (v[0] == '0' || v[0] == '1')) {
                    int on = v[0] == '1';
                    if (kn == 11 && strncmp(k, "swap_sticks", 11) == 0) { r->swap_sticks = (uint8_t)on; used = 1; }
                    else if (kn == 9 && strncmp(k, "invert_lx", 9) == 0) { r->invert_lx = (uint8_t)on; used = 1; }
                    else if (kn == 9 && strncmp(k, "invert_ly", 9) == 0) { r->invert_ly = (uint8_t)on; used = 1; }
                    else if (kn == 9 && strncmp(k, "invert_rx", 9) == 0) { r->invert_rx = (uint8_t)on; used = 1; }
                    else if (kn == 9 && strncmp(k, "invert_ry", 9) == 0) { r->invert_ry = (uint8_t)on; used = 1; }
                }
            }
        }
        p = *nl ? nl + 1 : nl;
    }
    return used || !written;
}

int remap_format(const remap_t *r, char *out, size_t max)
{
    size_t n = 0;
    int i, k;

#define PUT(...) do { k = snprintf(out + n, max - n, __VA_ARGS__); \
                      if (k < 0 || (size_t)k >= max - n) return 0; n += (size_t)k; } while (0)
    out[0] = '\0';
    for (i = 0; i < REMAP_BUTTONS; i++)
        if (r->to[i] != i) PUT("%s = %s\n", remap_names[i], r->to[i] < REMAP_BUTTONS ? remap_names[r->to[i]] : "none");
    if (r->swap_sticks) PUT("swap_sticks = 1\n");
    if (r->invert_lx) PUT("invert_lx = 1\n");
    if (r->invert_ly) PUT("invert_ly = 1\n");
    if (r->invert_rx) PUT("invert_rx = 1\n");
    if (r->invert_ry) PUT("invert_ry = 1\n");
#undef PUT
    return 1;
}

int remap_from_query(remap_t *r, const char *q)
{
    const char *p = q;

    remap_identity(r);
    while (*p) {
        const char *end = strchr(p, ';');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n >= 2 && strncmp(p, "m=", 2) == 0) {
            const char *s = p + 2, *stop = p + n;
            while (s < stop) {
                char *e1, *e2;
                long from = strtol(s, &e1, 10), to;
                if (e1 == s || *e1 != '.' || e1 >= stop) return 0;
                to = strtol(e1 + 1, &e2, 10);
                if (e2 == e1 + 1 || from < 0 || from >= REMAP_BUTTONS || to < 0 || to > REMAP_NONE) return 0;
                r->to[from] = (uint8_t)to;
                s = e2;
                if (s < stop) {
                    if (*s != ',') return 0;
                    s++;
                    if (s >= stop) return 0;        /* a trailing comma */
                }
            }
        } else if (n == 4 && (p[3] == '0' || p[3] == '1') && p[2] == '=') {
            int on = p[3] == '1';
            if (strncmp(p, "sw", 2) == 0) r->swap_sticks = (uint8_t)on;
            else return 0;
        } else if (n == 5 && (p[4] == '0' || p[4] == '1') && p[3] == '=' && p[0] == 'i') {
            int on = p[4] == '1';
            if (p[1] == 'l' && p[2] == 'x') r->invert_lx = (uint8_t)on;
            else if (p[1] == 'l' && p[2] == 'y') r->invert_ly = (uint8_t)on;
            else if (p[1] == 'r' && p[2] == 'x') r->invert_rx = (uint8_t)on;
            else if (p[1] == 'r' && p[2] == 'y') r->invert_ry = (uint8_t)on;
            else return 0;
        } else if (n != 0) {
            return 0;
        }
        p += n;
        if (*p == ';') p++;
    }
    return 1;
}

int remap_to_query(const remap_t *r, char *out, size_t max)
{
    size_t n = 0;
    int i, k, first = 1;

#define PUT(...) do { k = snprintf(out + n, max - n, __VA_ARGS__); \
                      if (k < 0 || (size_t)k >= max - n) return 0; n += (size_t)k; } while (0)
    out[0] = '\0';
    PUT("m=");
    for (i = 0; i < REMAP_BUTTONS; i++) {
        PUT("%s%d.%d", first ? "" : ",", i, r->to[i]);
        first = 0;
    }
    PUT(";sw=%d;ilx=%d;ily=%d;irx=%d;iry=%d", r->swap_sticks, r->invert_lx, r->invert_ly,
        r->invert_rx, r->invert_ry);
#undef PUT
    return 1;
}

static void remap_path(char *out, size_t max, const char *dir, unsigned vid, unsigned pid)
{
    snprintf(out, max, "%s/%04x_%04x.remap", dir, vid & 0xFFFF, pid & 0xFFFF);
}

int remap_load(remap_t *r, const char *dir, unsigned vid, unsigned pid)
{
    char path[320], text[2048];
    FILE *f;
    size_t n;

    remap_identity(r);
    remap_path(path, sizeof path, dir, vid, pid);
    if (!(f = fopen(path, "r"))) return 0;
    n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = '\0';
    return remap_parse(r, text);
}

int remap_save(const remap_t *r, const char *dir, unsigned vid, unsigned pid)
{
    char path[320], tmp[330], text[2048];
    FILE *f;

    remap_path(path, sizeof path, dir, vid, pid);
    if (remap_is_identity(r)) {
        unlink(path);
        return 1;
    }
    if (!remap_format(r, text, sizeof text)) return 0;
    mkdir(dir, 0755);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return 0;
    if (fputs(text, f) == EOF || fclose(f) != 0) {
        unlink(tmp);
        return 0;
    }
    return rename(tmp, path) == 0;
}
