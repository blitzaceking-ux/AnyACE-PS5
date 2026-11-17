/* A per-pad remapping, applied to every pad_state after the profile has
 * parsed it and before the console sees it: each button the pad sends can
 * become any PlayStation button (or nothing), the sticks can be swapped and
 * each axis inverted. Works the same for every profile and for wired pads.
 *
 * Kept as text, one setting per line, in <dir>/VVVV_PPPP.remap (hex ids):
 *
 *   cross = circle          the pad's cross acts as circle
 *   l2 = none               the pad's L2 does nothing
 *   swap_sticks = 1
 *   invert_ly = 1           left stick up/down reversed (also lx, rx, ry)
 *
 * Buttons not mentioned keep their meaning. The page sends the same thing
 * in one line ("m=0.1,1.0;sw=1;ily=1", indexes into remap_names). */
#ifndef ANYPAD_REMAP_H
#define ANYPAD_REMAP_H

#include "pad.h"

#include <stddef.h>
#include <stdint.h>

#define REMAP_BUTTONS 18
#define REMAP_NONE    REMAP_BUTTONS

extern const char *const remap_names[REMAP_BUTTONS];    /* "cross", "circle", ... */
extern const uint32_t remap_bits[REMAP_BUTTONS];        /* PAD_CROSS, PAD_CIRCLE, ... */

typedef struct {
    uint8_t to[REMAP_BUTTONS];      /* index of what each button becomes, or REMAP_NONE */
    uint8_t swap_sticks;
    uint8_t invert_lx, invert_ly, invert_rx, invert_ry;
} remap_t;

void remap_identity(remap_t *r);
int  remap_is_identity(const remap_t *r);

/* Applies r to st in place. */
void remap_apply(const remap_t *r, pad_state *st);

/* The text form (above). Parsing takes what it understands and ignores the
 * rest; returns 0 only for text with nothing usable where something was
 * written. Formatting writes only what differs from the identity. */
int  remap_parse(remap_t *r, const char *text);
int  remap_format(const remap_t *r, char *out, size_t max);

/* The page's one-line form: "m=<from>.<to>,...;sw=1;ilx=1;ily=1;irx=1;iry=1".
 * Returns 0 if anything in it is malformed. */
int  remap_from_query(remap_t *r, const char *q);
int  remap_to_query(const remap_t *r, char *out, size_t max);

/* <dir>/VVVV_PPPP.remap. Loading a missing file gives the identity and
 * returns 0; saving the identity removes the file. */
int  remap_load(remap_t *r, const char *dir, unsigned vid, unsigned pid);
int  remap_save(const remap_t *r, const char *dir, unsigned vid, unsigned pid);

#endif
