/* Per-pad remapping: applying it, both text forms, and the file. */
#include "../src/remap.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static void test_apply(void)
{
    remap_t r;
    pad_state st;

    printf("applying a remapping\n");
    remap_identity(&r);
    CHECK(remap_is_identity(&r));
    pad_state_reset(&st);
    st.buttons = PAD_CROSS | PAD_L2;
    st.l2 = 200;
    remap_apply(&r, &st);
    CHECK(st.buttons == (PAD_CROSS | PAD_L2) && st.l2 == 200);     /* identity: untouched */

    /* cross <-> circle, L1 becomes R2 (full press), L2 becomes nothing. */
    r.to[0] = 1; r.to[1] = 0; r.to[4] = 7; r.to[6] = REMAP_NONE;
    CHECK(!remap_is_identity(&r));
    pad_state_reset(&st);
    st.buttons = PAD_CROSS | PAD_L1 | PAD_L2 | PAD_UP;
    st.l2 = 200;
    remap_apply(&r, &st);
    CHECK(st.buttons == (PAD_CIRCLE | PAD_R2 | PAD_UP));
    CHECK(st.l2 == 0 && st.r2 == 255);

    /* Triggers swapped keep their travel. */
    remap_identity(&r);
    r.to[6] = 7; r.to[7] = 6;
    pad_state_reset(&st);
    st.buttons = PAD_L2;
    st.l2 = 90; st.r2 = 0;
    remap_apply(&r, &st);
    CHECK(st.buttons == PAD_R2 && st.r2 == 90 && st.l2 == 0);

    /* Sticks swapped and inverted. */
    remap_identity(&r);
    r.swap_sticks = 1; r.invert_ly = 1;
    pad_state_reset(&st);
    st.lx = 10; st.ly = 20; st.rx = 30; st.ry = 40;
    remap_apply(&r, &st);
    CHECK(st.lx == 30 && st.ly == 255 - 40 && st.rx == 10 && st.ry == 20);
}

static void test_text(void)
{
    remap_t r, back;
    char text[2048], q[512];

    printf("the text form and the page's one-line form\n");
    CHECK(remap_parse(&r, "# a comment\ncross = circle\n circle=cross \nl2 = none\nswap_sticks = 1\ninvert_ry=1\nbogus = thing\n"));
    CHECK(r.to[0] == 1 && r.to[1] == 0 && r.to[6] == REMAP_NONE && r.swap_sticks && r.invert_ry && !r.invert_ly);
    CHECK(r.to[2] == 2);
    CHECK(remap_format(&r, text, sizeof text));
    CHECK(strcmp(text, "cross = circle\ncircle = cross\nl2 = none\nswap_sticks = 1\ninvert_ry = 1\n") == 0);
    CHECK(remap_parse(&back, text) && memcmp(&back, &r, sizeof r) == 0);
    CHECK(remap_parse(&back, "") && remap_is_identity(&back));
    CHECK(!remap_parse(&back, "nothing useful here\n"));

    CHECK(remap_to_query(&r, q, sizeof q));
    CHECK(strncmp(q, "m=0.1,1.0,2.2,3.3,4.4,5.5,6.18,7.7,", 35) == 0);
    CHECK(strstr(q, ";sw=1;ilx=0;ily=0;irx=0;iry=1") != NULL);
    CHECK(remap_from_query(&back, q) && memcmp(&back, &r, sizeof r) == 0);
    CHECK(remap_from_query(&back, "m=0.1;sw=1") && back.to[0] == 1 && back.swap_sticks);
    CHECK(remap_from_query(&back, "") && remap_is_identity(&back));
    CHECK(!remap_from_query(&back, "m=0.99"));
    CHECK(!remap_from_query(&back, "m=x"));
    CHECK(!remap_from_query(&back, "zz=1"));
    CHECK(!remap_from_query(&back, "m=0.1,"));
}

static void test_file(void)
{
    remap_t r, back;

    printf("the file per pad\n");
    unlink("build/remap/054c_05c4.remap");
    remap_identity(&r);
    r.to[3] = 2;
    CHECK(remap_save(&r, "build/remap", 0x054C, 0x05C4));
    CHECK(remap_load(&back, "build/remap", 0x054C, 0x05C4) && back.to[3] == 2);
    CHECK(!remap_load(&back, "build/remap", 0x1234, 0x5678) && remap_is_identity(&back));
    remap_identity(&r);
    CHECK(remap_save(&r, "build/remap", 0x054C, 0x05C4));          /* identity: file removed */
    CHECK(access("build/remap/054c_05c4.remap", F_OK) != 0);
}

int main(void)
{
    test_apply();
    test_text();
    test_file();
    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("all remap checks passed\n");
    return 0;
}
