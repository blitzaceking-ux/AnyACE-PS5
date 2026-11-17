/* A simulated Bluetooth controller with one game pad in range, behind the
 * hci_t interface. It answers HCI commands as a controller would, and
 * plays the pad's side of pairing, L2CAP, SDP and HID, so the host stack
 * can be exercised end to end without hardware. */
#ifndef PH_SIM_H
#define PH_SIM_H

#include "../src/hci.h"

#include <stdint.h>

typedef struct sim sim_t;

typedef enum {
    SIM_WAIT_PAIRING,   /* in pairing mode: waits for the host to find it */
    SIM_RECONNECT,      /* paired before: connects to the host by itself */
} sim_mode;

typedef struct {
    sim_mode mode;
    unsigned char addr[6];
    uint32_t cod;
    /* Its name: sent in the extended inquiry response if name_in_eir, else
     * only on a Remote Name Request. "": it does not answer one. */
    char name[64];
    int name_in_eir;
    /* In pairing mode it also calls the host by itself every few seconds,
     * as a pad that remembers the host does (8BitDo); while that call is
     * pending, the host's own page to it fails with "already exists". */
    int pages_host;
    /* It never answers a page. */
    int no_answer;
    /* A stale link (sim_stale_link) is reported the way the console does it:
     * the page is accepted, then Connection Complete says 0x0b; an Accept
     * Connection with no call pending is refused with 0x0c. */
    int stale_in_complete;
    /* It takes a page only this soon (ms) after answering an inquiry. 0: any time. */
    int page_window_ms;
    /* pages_host, but the host never hears the call (the event goes to the
     * system's driver); answering blindly still works. */
    int call_unheard;
    uint16_t vid, pid;
    /* The input report the pad streams once its interrupt channel is open
     * and it has seen an output report (or at once, if !needs_output). */
    unsigned char report[128];
    int report_len;
    int needs_output;
    /* Streamed before the first output report, if any (a pad's simple
     * mode before it is woken up). */
    unsigned char basic[64];
    int basic_len;
    /* The HID report descriptor its SDP record serves, in pieces. */
    unsigned char desc[512];
    int desc_len;
    /* Drop every Nth event and ACL packet, as the system's driver would
     * take them. 0: none. */
    int drop_every;
    unsigned seed;
} sim_pad;

sim_t *sim_new(const sim_pad *pad);
void   sim_free(sim_t *s);
hci_t  sim_hci(sim_t *s);

/* Virtual clock, advanced by each pump. */
long   sim_now(void);
int    sim_diag_calls(void);   /* times the host asked the transport for its self-report */

void   sim_power_on(sim_t *s);      /* SIM_RECONNECT: page the host */
void   sim_disconnect(sim_t *s);    /* the pad switches off */
void   sim_set_report(sim_t *s, const unsigned char *r, int len);
void   sim_give_key(sim_t *s, const unsigned char *key);   /* paired before */
void   sim_go_deaf(sim_t *s);
/* Garbage from the air: frames and events no well-behaved pad sends. */
void   sim_inject_acl(sim_t *s, unsigned cid, const unsigned char *d, int n);
void   sim_inject_event(sim_t *s, unsigned char code, const unsigned char *p, int n);    /* stops answering, as after rest mode */

/* What the host did. */
int    sim_sdp_queries(const sim_t *s);
int    sim_outputs(const sim_t *s);
int    sim_sdp_continuations(const sim_t *s);
int    sim_last_output(const sim_t *s, unsigned char *out, int max);
int    sim_link_up(const sim_t *s);
int    sim_dropped(const sim_t *s);
int    sim_scan(const sim_t *s);           /* the controller's scan enable */
void   sim_set_scan(sim_t *s, int v);     /* someone else changes it */
void   sim_stale_link(sim_t *s, unsigned handle);   /* the chip still holds the pad's link from an earlier run */
int    sim_stale_cleared(const sim_t *s);
int    sim_name_requests(const sim_t *s);
int    sim_pages(const sim_t *s);      /* Create Connection commands the host sent */

#endif
