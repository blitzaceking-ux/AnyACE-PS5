/* AnyPad PS5 -- Bluetooth game controllers on the PlayStation 5.
 *
 * Pads from other consoles and brands connect to the console's own
 * Bluetooth chip, beside the DualSense, and appear to games as DualSense
 * controllers of the main user.
 *
 *   hci_usb.c   the Bluetooth chip, shared with the system's driver
 *   host.c      pairing, reconnection, L2CAP, SDP and HID (Classic)
 *   le.c        the same over Bluetooth LE
 *   profiles.c  each controller's reports
 *   ps5_vpad.c  the virtual DualSense each pad drives
 *   web.c       the menu: a page on port 8095
 *   (no console hotkey: a plain payload cannot read the DualSense; the menu opens from the media row)
 *
 * ALPHA: see PRUEBAS.md for what has been tried on a console.
 *
 * Stability comes first. It only touches what it created: its own Bluetooth
 * links, its own virtual pads, its own files under /data/anypad. When
 * something it depends on fails, it says so (log, notification, the menu)
 * and stays out of the way instead of retrying forever.
 *
 * The menu is started first, before anything that can fail, so that it is
 * there to explain a failure. It is a web page: from a phone or a PC at the
 * address the start-up notification gives, or on the console itself by
 * holding a button combination on the DualSense (L2 + R2 + OPTIONS for 1.5
 * seconds unless /data/anypad/config.ini says otherwise).
 *
 * Stopping: the menu's button, or create /data/anypad/stop. It disconnects
 * its pads, removes its virtual pads, puts back the controller setting it
 * changed, and exits. It does the same by itself when the console starts
 * going to rest mode (read from SceSystemStateMgrInfo), so nothing of it is left half-way across a sleep.
 * Pairing: the menu's button, or create /data/anypad/pair.
 */
#include "hci_usb.h"
#include "host.h"
#include "lock.h"
#include "log.h"
#include "netinfo.h"
#include "ps5_power.h"
#include "ps5_ui.h"
#include "ps5_apps.h"
#include "usb_pads.h"
#include "remap.h"
#include "ps5_sysinfo.h"
#include "ps5_vpad.h"
#include "util.h"
#include "version.h"
#include "web.h"

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define STATE_DIR    "/data/anypad"
#define LOG_PATH     STATE_DIR "/anypad.log"
#define DB_PATH      STATE_DIR "/pads.db"
#define LOCK_PATH    STATE_DIR "/anypad.lock"
#define CONFIG_PATH  STATE_DIR "/config.ini"
#define PAIR_FLAG    STATE_DIR "/pair"
#define STOP_FLAG    STATE_DIR "/stop"
#define NO_ICON_FLAG STATE_DIR "/no_icon"          /* do not put AnyPad in the media row */
#define RM_ICON_FLAG STATE_DIR "/remove_icon"      /* take it out, once */
#define PAIR_SECONDS 60

#define OPEN_TRIES      5       /* opening Bluetooth: 5 tries, 3 s apart */
#define RAISED_TRIES    2       /* ... the first two with raised credentials */

/* Findable in the binary: strings AnyACE-PS5-*.elf | grep anypad-version */
static const char g_version_tag[] __attribute__((used)) = "anypad-version " ANYPAD_VERSION;

static volatile sig_atomic_t g_stop;
static volatile int g_web_stop;         /* the menu's stop button */
static volatile int g_retry;            /* the menu's retry button */
static volatile int g_bt_state = WEB_BT_TRYING;
static char g_bt_reason[200];

static host_t *g_host;                  /* NULL until Bluetooth is up */
static web_t *g_web;
static int g_vpad_ok;                   /* virtual pads are possible */
static int g_bt_attempt;                /* Bluetooth tries made in this round */
static long g_bt_next;                  /* when the next one is due */
static int g_bt_plain_creds;            /* the chip answered only without raised credentials */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- virtual pads follow the controllers ---------------------------------- */

/* ---- settings: /data/anypad/settings.txt, one "key=value" per line -------
 *   auto_ps=0|1          press PS on a pad as soon as it connects
 *   bind_user=auto|<hex> which user the pads are given to (also written to
 *                        the bind_user file that ps5_vpad.c reads) */
#define PS_HOLD_MS 700                  /* how long PS is held, from the menu or automatically */
#define SETTINGS_FILE STATE_DIR "/settings.txt"
#define BIND_USER_FILE STATE_DIR "/bind_user"
static struct { int auto_ps; char bind_user[20]; } g_set = { 0, "auto" };
static long g_auto_ps_due[VPAD_MAX_SLOTS];     /* PS to press once the slot is live, 0: none */

static void settings_load(void)
{
    FILE *f = fopen(SETTINGS_FILE, "r");
    char line[64];

    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (strncmp(line, "auto_ps=", 8) == 0) g_set.auto_ps = line[8] == '1';
        else if (strncmp(line, "bind_user=", 10) == 0) snprintf(g_set.bind_user, sizeof g_set.bind_user, "%s", line + 10);
    }
    fclose(f);
    log_line("settings: auto_ps=%d bind_user=%s", g_set.auto_ps, g_set.bind_user);
}

static int settings_save(void)
{
    FILE *f = fopen(SETTINGS_FILE, "w");

    if (!f) return 0;
    fprintf(f, "auto_ps=%d\nbind_user=%s\n", g_set.auto_ps, g_set.bind_user);
    fclose(f);
    /* The user choice lives where the virtual pad code already looks. */
    if (strcmp(g_set.bind_user, "auto") == 0) {
        unlink(BIND_USER_FILE);
    } else if ((f = fopen(BIND_USER_FILE, "w")) != NULL) {
        fprintf(f, "%s\n", g_set.bind_user);
        fclose(f);
    }
    return 1;
}

static int web_settings_get(char *json, size_t max)
{
    vpad_user users[4];
    int n = g_vpad_ok ? vpad_users(users, 4) : 0, i, k;
    size_t used = 0;

#define PUT(...) do { k = snprintf(json + used, max - used, __VA_ARGS__); \
                      if (k < 0 || (size_t)k >= max - used) return 0; used += (size_t)k; } while (0)
    PUT("{\"auto_ps\":%s,\"bind_user\":\"%s\",\"users\":[", g_set.auto_ps ? "true" : "false", g_set.bind_user);
    for (i = 0; i < n; i++) {
        char name[48];
        size_t a = 0, b;
        for (b = 0; users[i].name[b] && a + 1 < sizeof name; b++)      /* JSON-safe */
            if ((unsigned char)users[i].name[b] >= 0x20 && users[i].name[b] != '"' && users[i].name[b] != '\\')
                name[a++] = users[i].name[b];
        name[a] = '\0';
        PUT("%s{\"id\":\"%x\",\"name\":\"%s\",\"initial\":%s}", i ? "," : "", (unsigned)users[i].id, name,
            users[i].initial ? "true" : "false");
    }
    PUT("]}");
#undef PUT
    return 1;
}

static int web_settings_set(const char *query)
{
    char user_before[20];
    const char *p;
    int i;

    snprintf(user_before, sizeof user_before, "%s", g_set.bind_user);
    if ((p = strstr(query, "auto_ps=")) != NULL) g_set.auto_ps = p[8] == '1';
    if ((p = strstr(query, "bind_user=")) != NULL) {
        char v[20];
        size_t n = strcspn(p + 10, "&");
        if (n == 0 || n >= sizeof v) return 0;
        memcpy(v, p + 10, n);
        v[n] = '\0';
        if (strcmp(v, "auto") != 0) {
            char *end;
            strtoul(v, &end, 16);
            if (*end) return 0;
        }
        snprintf(g_set.bind_user, sizeof g_set.bind_user, "%s", v);
    }
    if (!settings_save()) return 0;
    log_line("settings: auto_ps=%d bind_user=%s (from the page)", g_set.auto_ps, g_set.bind_user);
    if (strcmp(user_before, g_set.bind_user) != 0 && g_vpad_ok)
        for (i = 0; i < VPAD_MAX_SLOTS; i++) vpad_reassign(i);
    return 1;
}

/* Each slot's remapping, loaded for the pad's ids when it connects. */
#define REMAP_DIR STATE_DIR "/remap"
static struct { remap_t map; unsigned vid, pid; int used; } g_remap[VPAD_MAX_SLOTS];

static void on_connect(void *ud, int slot, const pad_info *info)
{
    (void)ud;
    if (slot >= 0 && slot < VPAD_MAX_SLOTS) {
        g_remap[slot].vid = info->vid;
        g_remap[slot].pid = info->pid;
        g_remap[slot].used = 1;
        if (remap_load(&g_remap[slot].map, REMAP_DIR, info->vid, info->pid))
            log_line("slot %d: remapping loaded for %04x:%04x", slot, info->vid, info->pid);
    }
    if (vpad_add(slot)) notify("AnyACE: %s connected", info->profile);
    else notify("AnyACE: %s connected, but without a virtual pad (see the log)", info->profile);
    if (g_set.auto_ps && slot >= 0 && slot < VPAD_MAX_SLOTS) g_auto_ps_due[slot] = now_ms() + 15000;
}

/* The automatic PS press, once the slot's virtual pad is live. */
static void auto_ps_tick(long now)
{
    int i;

    for (i = 0; i < VPAD_MAX_SLOTS; i++) {
        if (!g_auto_ps_due[i]) continue;
        if (now > g_auto_ps_due[i]) { g_auto_ps_due[i] = 0; continue; }     /* never came up */
        if (!vpad_live(i)) continue;
        g_auto_ps_due[i] = 0;
        if (vpad_press_ps(i, PS_HOLD_MS)) log_line("slot %d: PS pressed automatically", i);
    }
}

static void on_state(void *ud, int slot, const pad_state *st)
{
    pad_state mapped = *st;

    (void)ud;
    if (slot >= 0 && slot < VPAD_MAX_SLOTS && g_remap[slot].used) remap_apply(&g_remap[slot].map, &mapped);
    vpad_update(slot, &mapped);
}

static void web_remap_apply(int slot, pad_state *st)
{
    if (slot >= 0 && slot < VPAD_MAX_SLOTS && g_remap[slot].used) remap_apply(&g_remap[slot].map, st);
}

static int web_remap_get(int slot, char *out, size_t max)
{
    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !g_remap[slot].used) return 0;
    return remap_to_query(&g_remap[slot].map, out, max);
}

/* Sets the slot's remapping, saves it for the pad's ids, and gives it to
 * every connected pad with the same ids. */
static int web_remap_set(int slot, const char *query)
{
    remap_t r;
    int i;

    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !g_remap[slot].used) return 0;
    if (!remap_from_query(&r, query)) return 0;
    if (!remap_save(&r, REMAP_DIR, g_remap[slot].vid, g_remap[slot].pid)) {
        log_line("slot %d: remapping could not be saved under %s", slot, REMAP_DIR);
        return 0;
    }
    for (i = 0; i < VPAD_MAX_SLOTS; i++)
        if (g_remap[i].used && g_remap[i].vid == g_remap[slot].vid && g_remap[i].pid == g_remap[slot].pid)
            g_remap[i].map = r;
    log_line("slot %d: remapping %s for %04x:%04x", slot, remap_is_identity(&r) ? "cleared" : "saved",
             g_remap[slot].vid, g_remap[slot].pid);
    return 1;
}

/* The menu's PS button: the same as holding PS on that pad. */
static int web_press_ps(int slot)
{
    return g_vpad_ok && vpad_press_ps(slot, PS_HOLD_MS);
}

static void on_disconnect(void *ud, int slot)
{
    (void)ud;
    if (slot >= 0 && slot < VPAD_MAX_SLOTS) {
        g_remap[slot].used = 0;
        g_auto_ps_due[slot] = 0;
    }
    vpad_remove(slot);
    notify("AnyACE: pad %d disconnected", slot + 1);
}

/* Wired Xbox pads take the slots after the Bluetooth ones. */
static const host_events k_usb_events = { on_connect, on_state, on_disconnect, NULL, NULL };
#define USB_FIRST_SLOT HOST_MAX_PADS
#define BT_CHIP_NODE "/dev/ugen0.2"

/* /data/anypad/gip_keepalive holds a number of seconds: re-send the wired
 * pads' wake-up sequence that often (for a pad that switches itself off).
 * Absent or 0: never, since the sequence costs a moment of input. */
#define GIP_KEEPALIVE_FILE STATE_DIR "/gip_keepalive"
static int gip_keepalive_seconds(void)
{
    FILE *f = fopen(GIP_KEEPALIVE_FILE, "r");
    int v = 0;

    if (!f) return 0;
    if (fscanf(f, "%d", &v) != 1 || v < 0 || v > 3600) v = 0;
    fclose(f);
    if (v) log_line("usb pads: keepalive every %d s (%s)", v, GIP_KEEPALIVE_FILE);
    return v;
}

/* The menu's rumble test: both motors on for a moment, then off. Games'
 * own rumble does not reach a virtual pad yet (the console offers no way to
 * read it back), so this is the only rumble there is for now. */
#define RUMBLE_TEST_MS 600
static long g_rumble_off[VPAD_MAX_SLOTS];

static int set_rumble(int slot, uint8_t strong, uint8_t weak)
{
    pad_output o;

    if (slot < HOST_MAX_PADS) return g_host && host_rumble(g_host, slot, strong, weak);
    memset(&o, 0, sizeof o);
    o.strong = strong;
    o.weak = weak;
    return usb_pads_set_output(slot, &o);
}

static int web_rumble(int slot)
{
    if (slot < 0 || slot >= VPAD_MAX_SLOTS || !set_rumble(slot, 255, 255)) return 0;
    g_rumble_off[slot] = now_ms() + RUMBLE_TEST_MS;
    log_line("slot %d: rumble test", slot);
    return 1;
}

static void rumble_tick(long now)
{
    int i;

    for (i = 0; i < VPAD_MAX_SLOTS; i++)
        if (g_rumble_off[i] && now >= g_rumble_off[i]) {
            g_rumble_off[i] = 0;
            set_rumble(i, 0, 0);
        }
}

static int web_probe(int slot)
{
    return g_vpad_ok && vpad_probe_start(slot, 90);
}

static const char *web_probe_status(int slot)
{
    return g_vpad_ok ? vpad_probe_status(slot) : "";
}

static int web_rebind(int slot)
{
    return g_vpad_ok ? vpad_rebind(slot) : 0;
}

static const char *web_vpad_state(int slot)
{
    return g_vpad_ok ? vpad_status(slot) : "virtual pads unavailable";
}

static int web_wired_get(int i, web_wired *out)
{
    static const char *const names[] = { "", "starting", "live", "silent" };
    wired_pad_info info;

    if (!usb_pads_get(i, &info, &out->st)) return 0;
    snprintf(out->node, sizeof out->node, "%s", info.node);
    out->vid = info.vid;
    out->pid = info.pid;
    out->name = info.name;
    out->kind = info.kind;
    out->slot = info.slot;
    out->live = info.state == WIRED_ST_LIVE;
    out->state = names[info.state >= 0 && info.state <= 3 ? info.state : 0];
    out->reports = info.reports;
    return 1;
}

/* ---- Bluetooth, one try at a time ------------------------------------------ */

/* The host calls this whenever it waits for the chip, so the menu answers
 * even while a Bluetooth try is in progress (it used to freeze for ~5 s each
 * time, and the browser's pile-up of connections got "busy"). */
static void on_idle(void *ud)
{
    (void)ud;
    web_poll(g_web);
}

static void bt_begin(void)
{
    g_bt_attempt = 0;
    g_bt_next = 0;
    g_bt_reason[0] = '\0';
    g_bt_state = WEB_BT_TRYING;
    if (g_vpad_ok) vpad_creds_raise();
}

/* One try. The controller may answer only to a process that does not carry
 * raised credentials (not known), so the first
 * tries are made as they are and the rest with the original ones. Which
 * worked is logged. Virtual pads raise them again when they need to. */
static void bt_try(long now)
{
    host_events ev = { on_connect, on_state, on_disconnect, NULL, on_idle };
    hci_t hci;
    int attempt = ++g_bt_attempt;

    if (attempt == RAISED_TRIES + 1 && g_vpad_ok) {
        log_line("Bluetooth silent with raised credentials; trying with the original ones");
        if (vpad_creds_restore()) g_bt_plain_creds = 1;
    }
    if (hci_usb_open(&hci)) {
        g_host = host_open(hci, DB_PATH, &ev);
        if (g_host) {
            g_bt_state = WEB_BT_OK;
            log_line("Bluetooth ready after %d tr%s%s", attempt, attempt == 1 ? "y" : "ies",
                     g_bt_plain_creds ? ", answering only with the original credentials" : "");
            notify("AnyACE: Bluetooth ready (%d paired pads)", host_paired_count(g_host));
            return;
        }
        hci.ops->close(hci.ctx);
    }
    log_line("Bluetooth not ready (try %d of %d)", attempt, OPEN_TRIES);
    if (attempt >= OPEN_TRIES) {
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_bt_reason, sizeof g_bt_reason,
                 "The Bluetooth chip does not answer (%d tries, with and without raised credentials). "
                 "Details in the log, 'diag' lines.", attempt);
        log_line("Bluetooth unavailable: %s", g_bt_reason);
        notify("AnyACE: Bluetooth does not answer.\nStill running: open the menu to see why");
        return;
    }
    g_bt_next = now + 3000;
}

static void bt_lost(const char *why)
{
    log_line("Bluetooth lost: %s", why);
    host_close(g_host);                 /* drops every pad and its virtual pad */
    g_host = NULL;
    g_bt_state = WEB_BT_FAILED;
    snprintf(g_bt_reason, sizeof g_bt_reason, "Bluetooth was lost (%s). Press Retry.", why);
    notify("AnyACE: Bluetooth was lost.\nPress Retry in the menu");
}

/* ---- flags ---------------------------------------------------------------- */

static void check_flags(long now)
{
    static long last_second;

    if (now / 1000 == last_second) return;
    last_second = now / 1000;
    if (access(STOP_FLAG, F_OK) == 0) {
        unlink(STOP_FLAG);
        g_stop = 1;
    }
    if (g_host && access(PAIR_FLAG, F_OK) == 0) {
        unlink(PAIR_FLAG);
        host_pair(g_host, PAIR_SECONDS);
        notify("AnyACE: searching for pads for %d s.\nPut your pad in pairing mode, then pick it in the AnyACE menu", PAIR_SECONDS);
    }
}

int main(void)
{
    web_cfg wc;
    char ip[16], url[64];
    const char *why = "stop requested";
    long last_power = 0;

    mkdir(STATE_DIR, 0755);
    log_open(LOG_PATH);
    log_line("AnyACE %s", ANYPAD_VERSION);
    settings_load();
    (void)g_version_tag;

    if (!lock_take(LOCK_PATH)) {
        log_line("another instance is running");
        notify("AnyACE: already running");
        log_close();
        return 1;
    }
    unlink(STOP_FLAG);                  /* an old request is not for us */
    syscall(SYS_thr_set_name, -1, "anypad");    /* how others find us */
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);           /* a browser closing early */


    /* The menu first: it is what explains everything that follows. */
    memset(&wc, 0, sizeof wc);
    wc.host = &g_host;
    wc.stop = &g_web_stop;
    wc.retry = &g_retry;
    wc.bt_state = &g_bt_state;
    wc.bt_reason = g_bt_reason;
    wc.log_path = LOG_PATH;
    wc.version = ANYPAD_VERSION;
    wc.press_ps = web_press_ps;
    wc.wired = web_wired_get;
    wc.vpad_state = web_vpad_state;
    wc.rebind = web_rebind;
    wc.rumble = web_rumble;
    wc.probe = web_probe;
    wc.probe_status = web_probe_status;
    wc.settings_get = web_settings_get;
    wc.settings_set = web_settings_set;
    wc.remap_apply = web_remap_apply;
    wc.remap_get = web_remap_get;
    wc.remap_set = web_remap_set;
    g_web = web_start(&wc, WEB_PORT);

    local_ip(ip);
    snprintf(url, sizeof url, "http://%s:%d/", ip, WEB_PORT);
    if (g_web)
        notify("AnyACE %s\nMenu: %s (or Media > AnyACE)\nFirst create a new user and give it the pad when the console asks. Play with your own user and tap PS to use that pad", ANYPAD_VERSION, url);
    else
        notify("AnyACE %s\nCould not open the menu page (port %d in use?)",
               ANYPAD_VERSION, WEB_PORT);

    sysinfo_log();

    /* The entry in the media row that opens the menu in the browser. Done
     * before the credentials are raised for the pads, as Payload Manager does
     * it with its own; /data/anypad/no_icon skips it, remove_icon undoes it. */
    if (access(RM_ICON_FLAG, F_OK) == 0) {
        apps_remove_launcher();
        unlink(RM_ICON_FLAG);
    } else if (access(NO_ICON_FLAG, F_OK) != 0) {
        apps_install_launcher();
    }

    /* Without virtual pads there is nothing to connect controllers to:
     * Bluetooth is not touched at all, and the menu says why. */
    g_vpad_ok = vpad_init();
    if (!g_vpad_ok) {
        g_bt_state = WEB_BT_FAILED;
        snprintf(g_bt_reason, sizeof g_bt_reason,
                 "Virtual pads cannot be created on this system (see the log). Bluetooth was not touched.");
        notify("AnyACE: virtual pads cannot be created.\nSee the menu or the log. Bluetooth was not touched");
    } else {
        bt_begin();
        usb_pads_start(&k_usb_events, USB_FIRST_SLOT, BT_CHIP_NODE, STATE_DIR "/maps", gip_keepalive_seconds());
    }

    while (!g_stop && !g_web_stop) {
        long now;

        if (g_host) host_poll(g_host, 4);
        else usleep(4000);
        now = now_ms();
        web_poll(g_web);
        if (g_vpad_ok) {
            usb_pads_poll(now);
            vpad_poll(now);
            auto_ps_tick(now);
            rumble_tick(now);
        }

        /* Rest mode on its way: leave cleanly before it, not after. */
        if (now - last_power >= 200) {
            int ps = power_state();
            last_power = now;
            if (ps == POWER_GOING_TO_REST || ps == POWER_IN_REST) {
                why = "rest mode";
                notify("AnyACE: stopped by rest mode.\nLaunch it again after waking up");
                break;
            }
        }

        /* The Bluetooth: tries, loss, and the menu's retry button. */
        if (g_bt_state == WEB_BT_TRYING && now >= g_bt_next) bt_try(now);
        if (g_host && host_transport_lost(g_host)) bt_lost("no answer from the chip");
        if (g_retry) {
            g_retry = 0;
            if (g_vpad_ok && g_bt_state == WEB_BT_FAILED) {
                log_line("retrying Bluetooth at the user's request");
                bt_begin();
            } else {
                log_line("retry ignored (virtual pads not available, or not failed)");
            }
        }

        check_flags(now);
    }
    if (g_web_stop) why = "stop from the menu";
    else if (g_stop && strcmp(why, "stop requested") == 0) why = "stop requested (flag or signal)";

    log_line("stopping: %s", why);
    web_stop(g_web);
    usb_pads_stop();                    /* the wired pads; removes their virtual pads */
    host_close(g_host);                 /* our links; removes our virtual pads */
    g_host = NULL;
    if (g_vpad_ok) {
        vpad_creds_restore();           /* leave the credentials as found */
    }
    lock_release();
    log_line("stopped");
    log_close();
    notify("AnyACE: stopped");
    return 0;
}
