/* Bluetooth HID host for game controllers, on a controller shared with the
 * console's own driver.
 *
 * Runs on one thread: host_poll() pumps the transport, dispatches what
 * arrived and advances every link's state machine. Every step that needs a
 * reply is retried or timed out, since a few packets go to the system's
 * driver instead of here.
 *
 *   pairing:    host_pair() searches for pads in pairing mode and lists
 *               what it finds (host_found_get); nothing is paired until
 *               the user picks one with host_pair_addr(). Then it connects,
 *               pairs, reads its vendor/product id over SDP and opens the
 *               HID channels. The link key and ids are stored.
 *   reconnect:  a stored pad that is switched on connects by itself; it is
 *               accepted, authenticated with the stored key and its HID
 *               channels accepted or opened.
 */
#ifndef PH_HOST_H
#define PH_HOST_H

#include "hci.h"
#include "pad.h"

#define HOST_MAX_PADS 4
#define HOST_FOUND_NAME 63

typedef struct host host_t;

typedef struct {
    void (*on_connect)(void *ud, int slot, const pad_info *info);
    void (*on_state)(void *ud, int slot, const pad_state *st);
    void (*on_disconnect)(void *ud, int slot);
    void *ud;
    /* Called every time the host waits (at least every 50 ms), including
     * while it waits for the controller to answer at start-up, so that the
     * menu keeps serving. Must be quick and must not call into the host.
     * NULL: nothing to do. */
    void (*idle)(void *ud);
} host_events;

/* Prepares the controller and loads the paired pads from db_path. Returns
 * NULL if the controller does not answer. */
host_t *host_open(hci_t hci, const char *db_path, const host_events *ev);
void    host_close(host_t *h);

void host_poll(host_t *h, int timeout_ms);

/* Searches for pads in pairing mode for `seconds`, and lists them. */
int  host_pair(host_t *h, int seconds);

/* Why a found device is not offered for pairing without force. */
enum {
    HOST_OK = 0,
    HOST_BLOCK_DUALSENSE,       /* named like a DualSense: likely the console's own */
    HOST_BLOCK_CONSOLE,         /* the console's own driver has a link to it */
};

typedef struct {
    unsigned char addr[6];
    int le;
    uint32_t cod;               /* Classic class of device */
    int rssi;
    char name[HOST_FOUND_NAME + 1];   /* "" while unknown */
    int name_pending;           /* still being asked for */
    int block;                  /* HOST_OK or a HOST_BLOCK_* */
    int paired;                 /* paired before (its key would be replaced) */
} host_found_info;

/* The i-th device the current search found, newest last. Returns 0 past
 * the end. */
int  host_found_get(const host_t *h, int i, host_found_info *out);

/* Pairs with a found device. Returns HOST_PAIR_* below. A blocked device
 * is only paired with `force`. */
enum {
    HOST_PAIR_STARTED = 1,
    HOST_PAIR_UNKNOWN = 0,      /* not found, or not seen for a minute */
    HOST_PAIR_BLOCKED = -1,     /* blocked; ask the user, then force */
    HOST_PAIR_BUSY = -2,        /* no free slot, or already connecting */
};
int  host_pair_addr(host_t *h, const unsigned char addr[6], int force);

/* How the user's last pick is going, for the page. */
enum { HOST_PICK_NONE, HOST_PICK_CHECKING, HOST_PICK_PAIRING, HOST_PICK_DONE, HOST_PICK_FAILED };
enum {
    HOST_WHY_NONE,
    HOST_WHY_NO_ANSWER,         /* the pad did not answer the page */
    HOST_WHY_COLLISION,         /* the pad was connecting to the console at the same time */
    HOST_WHY_DUALSENSE,         /* its name turned out to be a DualSense */
    HOST_WHY_FAILED,            /* anything else */
    HOST_WHY_REFUSED,           /* the Bluetooth chip refused to start the connection */
};
typedef struct {
    unsigned char addr[6];
    char name[HOST_FOUND_NAME + 1];
    int state;                  /* HOST_PICK_* */
    int why;                    /* HOST_WHY_*, for HOST_PICK_FAILED */
    int tries;
} host_pick_info;

/* Returns 0 if nothing was picked in this search. */
int  host_pick_get(const host_t *h, host_pick_info *out);

/* Sends rumble and lights to the pad in `slot`. Returns 0 if it is not
 * connected or takes no output. */
int  host_set_output(host_t *h, int slot, const pad_output *out);

/* Rumble alone to the pad in `slot`, its lights left as they are. */
int  host_rumble(host_t *h, int slot, uint8_t strong, uint8_t weak);

int  host_connected(const host_t *h, int slot);
int  host_paired_count(const host_t *h);

/* The controller went away (rest mode): close and open again. */
int  host_transport_lost(const host_t *h);

/* ---- for the web page ---- */

typedef struct {
    unsigned char addr[6];      /* identity address */
    uint16_t vid, pid;
    int le;                     /* Bluetooth LE */
    int connected;
    char name[HOST_FOUND_NAME + 1];     /* its Bluetooth name, "" unknown */
} host_paired_info;

/* Seconds left in the pairing window, 0 if not pairing. */
int  host_pairing_left(const host_t *h);

/* The i-th paired pad (0 <= i < host_paired_count). Returns 0 past the end. */
int  host_paired_get(const host_t *h, int i, host_paired_info *out);

/* A connected, ready pad's identity and latest state. Returns 0 if none. */
int  host_pad_get(const host_t *h, int slot, pad_info *info, pad_state *st);

/* Drops the link to the pad in `slot` without forgetting it (it reconnects
 * when switched on again). Returns 0 if nothing was connected there. */
int  host_disconnect(host_t *h, int slot);

/* How many input reports the pad in `slot` has sent, and the id of the last. */
int  host_pad_stats(const host_t *h, int slot, long *reports, int *last_rid,
                    long *other, int *rid_other);

/* Forgets a paired pad: disconnects it if linked and drops its keys.
 * Returns 0 if it was not paired. */
int  host_forget(host_t *h, const unsigned char addr[6]);

#endif
