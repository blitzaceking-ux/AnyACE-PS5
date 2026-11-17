/* AnyPad PS5's web page: status, pairing, forgetting pads, stopping, the log.
 *
 * Served from the main loop without threads or blocking: at most WEB_CLIENTS
 * connections, requests of at most 8 KB, three seconds each.
 *
 * There is no PIN, by the user's choice: anyone on the local network can
 * open the page, pair, forget pads and stop AnyPad PS5. Nothing else.
 */
#ifndef PH_WEB_H
#define PH_WEB_H

#include "host.h"

#include <stddef.h>

#define WEB_PORT 8095
#define WEB_MAX_SLOTS 8             /* Bluetooth slots, then wired ones */

/* A wired (USB) pad, as the main program reports it. */
typedef struct {
    char node[32];
    unsigned vid, pid;
    const char *name;           /* "" if unknown */
    const char *kind;           /* "gip", "xinput", "receiver", "hid" */
    int slot;                   /* 0-based */
    int live;                   /* input flowing */
    const char *state;          /* "starting", "live", "silent" */
    long reports;
    pad_state st;
} web_wired;

typedef struct web web_t;

enum { WEB_BT_TRYING, WEB_BT_OK, WEB_BT_FAILED };

typedef struct {
    host_t **host;              /* the host; NULL until Bluetooth is up */
    volatile int *stop;         /* set to 1 to ask AnyPad PS5 to stop */
    volatile int *retry;        /* set to 1 to try the Bluetooth again */
    volatile int *bt_state;     /* a WEB_BT_* */
    const char *bt_reason;      /* why not, when it failed (plain text) */
    const char *log_path;       /* tail served at /api/log */
    const char *version;
    const char *combo_text;     /* the console hotkey, in words */
    /* The user's own pad: returns 1 and its buttons if readable, else 0 and
     * the reason in *err. NULL: not available. */
    int (*physical)(uint32_t *buttons, int *err);
    /* Presses PS on pad `slot` (0-based). Returns 1 if done. NULL: not available. */
    int (*press_ps)(int slot);
    /* The i-th wired pad; returns 0 past the end. NULL: no wired pads. */
    int (*wired)(int i, web_wired *out);
    /* The slot's virtual pad on the console: "ready", "creating", or why
     * not. NULL: not available (reported as ""). */
    const char *(*vpad_state)(int slot);
    /* Retries giving the slot's virtual pad to a user. Returns 1 bound, 2
     * being made again, 0 nothing to retry. NULL: not available. */
    int (*rebind)(int slot);
    /* The slot's remapping: applied to a state (as the console sees it),
     * read in the page's one-line form, and set from it (0: bad or no
     * such pad). NULL: no remapping. */
    /* Settings as JSON (get) and from "auto_ps=1&bind_user=auto|<hex>" (set,
     * 0: bad). NULL: no settings. */
    /* A short rumble on the slot's pad, to test it. 0: no such pad. */
    int  (*rumble)(int slot);
    /* The remote-setting probe (an experiment): start it, and its status. */
    int  (*probe)(int slot);
    const char *(*probe_status)(int slot);
    int  (*settings_get)(char *json, size_t max);
    int  (*settings_set)(const char *query);
    void (*remap_apply)(int slot, pad_state *st);
    int  (*remap_get)(int slot, char *out, size_t max);
    int  (*remap_set)(int slot, const char *query);
} web_cfg;

/* Listens on `port` on every interface. Returns NULL if it cannot. */
web_t *web_start(const web_cfg *cfg, int port);
void   web_poll(web_t *w);
void   web_stop(web_t *w);

#endif
