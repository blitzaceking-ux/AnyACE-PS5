#include "ps5_vpad.h"
#include "host.h"
#include "log.h"
#include "util.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---- ScePadData ------------------------------------------------------------
 *
 * As PS4/PS5 software sees it: 120 bytes, with acceleration before
 * angular velocity. */

typedef struct {
    uint16_t x, y;
    uint8_t  id;
    uint8_t  reserved[3];
} ScePadTouch;

typedef struct {
    uint8_t  touchNum;
    uint8_t  reserved[3];
    uint32_t reserved2;
    ScePadTouch touch[2];
} ScePadTouchData;

typedef struct {
    uint32_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  l2, r2;
    uint8_t  padding[2];
    float    orientation[4];        /* x, y, z, w */
    float    acceleration[3];       /* g */
    float    angularVelocity[3];    /* rad/s */
    ScePadTouchData touchData;
    uint8_t  connected;
    uint64_t timestamp;             /* microseconds */
    uint8_t  extensionUnitData[16];
    uint8_t  connectedCount;
    uint8_t  reserved[2];
    uint8_t  deviceUniqueDataLen;
    uint8_t  deviceUniqueData[12];
} ScePadData;

_Static_assert(sizeof(ScePadData) == 120, "ScePadData must be 120 bytes");
_Static_assert(offsetof(ScePadData, touchData) == 52, "touch data offset");
_Static_assert(offsetof(ScePadData, timestamp) == 80, "timestamp offset");

/* ---- system interfaces --------------------------------------------------- */

#define VIRTUAL_DEVICE_DUALSENSE 3

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user);
int32_t sceUserServiceGetForegroundUser(int32_t *user);
int32_t sceUserServiceGetLoginUserIdList(int32_t list[4]);
int32_t sceUserServiceGetUserName(int32_t user, char *name, size_t size);
int32_t scePadInit(void);
int32_t scePadSetProcessPrivilege(int32_t privilege);
int32_t scePadGetHandle(int32_t user, int32_t type, int32_t index);
int32_t scePadOpen(int32_t user, int32_t type, int32_t index, const void *param);
int32_t scePadClose(int32_t handle);
int32_t scePadReadState(int32_t handle, ScePadData *data);
int32_t scePadVirtualDeviceAddDevice(void *param, int32_t deviceType);
int32_t scePadVirtualDeviceInsertData(int32_t handle, const void *data);
int32_t scePadVirtualDeviceDeleteDevice(int32_t handle);

/* From the payload SDK (ps5/kernel.h). */
uint64_t kernel_get_ucred_authid(pid_t pid);
int32_t  kernel_set_ucred_authid(pid_t pid, uint64_t authid);
int32_t  kernel_get_ucred_caps(pid_t pid, uint8_t caps[16]);
int32_t  kernel_set_ucred_caps(pid_t pid, const uint8_t caps[16]);

typedef int32_t (*mbus_bind_fn)(uint64_t device_id, int32_t user);

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request;

int sceKernelSendNotificationRequest(int, notify_request *, size_t, int);

/* ---- state ------------------------------------------------------------------ */

#define T_IDENTIFY      3000    /* AddDevice to its DEVICE_ADDED line */
#define T_AMBIGUITY      300    /* after a match, watch for a second one */
#define LINE_MAX_LEN    1024

/* VP_UNBOUND: the device exists and is fed, but the console did not give it
 * to a user (0x809b0081). It is left alive, so the console's own "who is
 * using this controller" prompt can assign it, and binding is tried again. */
enum { VP_FREE, VP_QUEUED, VP_PENDING, VP_READY, VP_FAILED, VP_UNBOUND };

#define BIND_USER_NOT_LOGIN 0x809B0081u

enum { VF_NONE, VF_USER, VF_IDENT, VF_REFUSED, VF_CREDS, VF_LOG };

#define T_USER_RETRY 10000      /* a pad refused for want of a user is tried again this often */

typedef struct {
    int st;
    int why;                    /* VF_*, for VP_FAILED */
    long t_fail;
    int remove_when_found;      /* removed while being identified */
    int32_t handle;
    uint64_t dev;
    int32_t user;               /* the user it was bound to, -1 */
    pad_state last;             /* the pad's latest state, resent while PS is held from the menu */
    long ps_until;              /* ms; PS is added to the frames until then (0: not held) */
    int ps_active;              /* a frame with PS went out and the release is still to be sent */
} vslot;

static mbus_bind_fn g_bind;
static int g_ready;                         /* vpad_init succeeded */
static int32_t g_user = -1;
static vslot g_slot[VPAD_MAX_SLOTS];
static void ps_tick(long now);
static void probe_tick(long now);

/* The identification under way: one at a time, so a DEVICE_ADDED line can
 * only belong to the device just asked for. */
static int g_pending = -1;
static long g_t_add, g_t_match;
static int g_matches;
static uint64_t g_match_dev;
static int g_log_fd = -1, g_log_is_dev;
static char g_line[LINE_MAX_LEN];
static size_t g_line_len;

/* Raw sensor counts to physical units, uncalibrated: Sony pads report
 * 8192 counts per g and 1024 per degree per second. */
#define ACCEL_PER_G      8192.0f
#define GYRO_PER_DEG_S   1024.0f
#define DEG_TO_RAD       0.01745329252f

void notify(const char *fmt, ...)
{
    notify_request req;
    va_list ap;

    memset(&req, 0, sizeof req);
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof req.message, fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* ---- the kernel log ----------------------------------------------------------
 *
 * The device a virtual DualSense becomes is only told in the kernel log. The
 * log has one reader, which usually belongs to the HEN's log server; that
 * server re-serves it to any number of clients on TCP 3232. So:
 *   - the server is used when there is one;
 *   - /dev/klog only when there is none, held just for the seconds of one
 *     identification, and every line read is copied into AnyPad PS5's log so
 *     nothing taken from it is lost;
 *   - if neither answers with the expected lines, nothing is bound. */

static int log_source_open(void)
{
    struct sockaddr_in sin;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd >= 0) {
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons(3232);
        sin.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(fd, (struct sockaddr *)&sin, sizeof sin) == 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            g_log_is_dev = 0;
            return fd;
        }
        close(fd);
    }
    fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        g_log_is_dev = 1;
        return fd;
    }
    log_line("vpad: no kernel log (no server on 3232, /dev/klog errno %d)", errno);
    return -1;
}

static void log_source_close(void)
{
    if (g_log_fd >= 0) close(g_log_fd);
    g_log_fd = -1;
    g_line_len = 0;
}

static uint64_t hex_after(const char *s, const char *key)
{
    const char *p = strstr(s, key);
    uint64_t v = 0;
    int digits = 0;

    if (!p) return 0;
    p += strlen(key);
    while (*p == ':' || *p == '=' || *p == ' ') p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (isxdigit((unsigned char)*p) && digits++ < 16) {
        char c = *p++;
        v = v << 4 | (uint64_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return v;
}

/* A virtual DualSense appearing:
 *   SCE_MBUS_EVENT_DEVICE_ADDED [DeviceId:0x...][type:1][subType:22]
 *   [capabilityBattery:0]...
 * A real pad reports a battery, so a line saying otherwise is not ours. */
static uint64_t virtual_device_in(const char *line)
{
    const char *bat, *type;

    /* Up to 11.xx: ...DEVICE_ADDED [DeviceId:0x...][type:1][subType:22]...
     * Reported on 13.60: ...DEVICE_ADDED: DeviceId:0x... subType:22 ...
     * (no type token). The subType is what identifies a virtual DualSense;
     * a type token, when there is one, must say 1. */
    if (!strstr(line, "DEVICE_ADDED") || !strstr(line, "subType:22")) return 0;
    if ((type = strstr(line, "[type:")) != NULL && strncmp(type, "[type:1]", 8) != 0) return 0;
    bat = strstr(line, "capabilityBattery:");
    if (bat && bat[18] != '0') return 0;
    return hex_after(line, "DeviceId");
}

/* Reads what the log has, a bounded amount per call. */
static void log_source_read(void)
{
    char buf[1024];
    int rounds;

    for (rounds = 0; rounds < 8 && g_log_fd >= 0; rounds++) {
        ssize_t n = read(g_log_fd, buf, sizeof buf), i;
        if (n <= 0) return;
        for (i = 0; i < n; i++) {
            uint64_t dev;
            if (buf[i] != '\n' && g_line_len < sizeof g_line - 1) {
                g_line[g_line_len++] = buf[i];
                continue;
            }
            g_line[g_line_len] = '\0';
            g_line_len = 0;
            if (g_log_is_dev && (strstr(g_line, "MBUS") || strstr(g_line, "DeviceId") ||
                                 strstr(g_line, "LoginMgr") || strstr(g_line, "SetDeviceIndex")))
                log_line("klog: %s", g_line);   /* the lines about pads and users, not the rest */
            dev = virtual_device_in(g_line);
            if (!dev || g_pending < 0) continue;
            if (g_matches == 0) {
                g_match_dev = dev;
                g_t_match = now_ms();
            }
            if (g_matches == 0 || dev != g_match_dev) g_matches++;
        }
    }
}

/* ---- users and privileges ---------------------------------------------------- */

/* The user to give the pads to: one who is signed in. scePadOpen and the
 * bind answer 0x809b0081 (USER_NOT_LOGIN) for a user who is not, and the
 * foreground or initial user is not necessarily signed in (the first trial
 * of the pad hotkey on a console got exactly that). By default the initial
 * user, who is the one who owns the console, is preferred if signed in; then
 * the foreground user; then the first signed-in one.
 *
 * `want` (from /data/anypad/bind_user) overrides that for an experiment: a
 * game only sees a pad bound to the user who started it, so the pad can be
 * given to another signed-in user. "other" means a signed-in user who is not
 * the initial one; a hexadecimal id means that user. If the wish cannot be met
 * (nobody else signed in, or that id is not signed in) the default applies. */
int32_t vpad_choose_user(const int32_t login[4], int32_t init, int32_t fore, const char *want)
{
    int i, n = 0;

    for (i = 0; i < 4; i++) if (login[i] != -1 && login[i] != 0) n++;
    if (n == 0) return -1;                                       /* nobody is signed in */

    if (want && *want) {
        if (strcmp(want, "other") == 0) {
            for (i = 0; i < 4; i++)
                if (login[i] != -1 && login[i] != 0 && login[i] != init) return login[i];
        } else {
            char *end;
            unsigned long id = strtoul(want, &end, 16);
            if (end != want && *end == '\0')
                for (i = 0; i < 4; i++) if (login[i] != -1 && (uint32_t)login[i] == (uint32_t)id) return login[i];
        }
    }
    for (i = 0; i < 4; i++) if (init != -1 && login[i] == init) return init;
    for (i = 0; i < 4; i++) if (fore != -1 && login[i] == fore) return fore;
    for (i = 0; i < 4; i++) if (login[i] != -1 && login[i] != 0) return login[i];
    return -1;
}

static int32_t main_user(void)
{
    int32_t login[4] = { -1, -1, -1, -1 }, init = -1, fore = -1, seen[6];
    static int32_t logged[6] = { -2, -2, -2, -2, -2, -2 };
    char want[40] = "";
    FILE *f;
    int i;

    if (sceUserServiceGetLoginUserIdList(login) != 0) login[0] = login[1] = login[2] = login[3] = -1;
    if (sceUserServiceGetInitialUser(&init) != 0) init = -1;
    if (sceUserServiceGetForegroundUser(&fore) != 0) fore = -1;
    if ((f = fopen("/data/anypad/bind_user", "r"))) {
        if (fgets(want, sizeof want, f)) {
            for (i = 0; want[i]; i++) if (want[i] == '\n' || want[i] == '\r' || want[i] == ' ') want[i] = 0;
            if (strncmp(want, "0x", 2) == 0) memmove(want, want + 2, strlen(want + 2) + 1);
        }
        fclose(f);
    }

    memcpy(seen, login, sizeof login);
    seen[4] = init;
    seen[5] = fore;
    if (memcmp(seen, logged, sizeof seen) != 0) {                /* log it when it changes */
        memcpy(logged, seen, sizeof seen);
        log_line("users: initial %#x, foreground %#x, signed in %#x %#x %#x %#x%s%s", (unsigned)init,
                 (unsigned)fore, (unsigned)login[0], (unsigned)login[1], (unsigned)login[2],
                 (unsigned)login[3], want[0] ? ", bind_user=" : "", want);
    }
    return vpad_choose_user(login, init, fore, want);
}

/* System-service credentials: MBus refuses a plain payload. Every step is
 * checked and read back; on any failure the original credentials are put
 * back and virtual pads stay off. */
static uint64_t g_orig_authid;
static uint8_t g_orig_caps[16];
static int g_have_orig;
static int g_raised;

/* Sets credentials and reads them back. Returns 0 if they did not stick. */
static int set_creds(uint64_t authid, const uint8_t caps[16])
{
    pid_t me = getpid();
    uint8_t now_caps[16];

    return kernel_set_ucred_authid(me, authid) == 0 && kernel_set_ucred_caps(me, caps) == 0 &&
           kernel_get_ucred_authid(me) == authid && kernel_get_ucred_caps(me, now_caps) == 0 &&
           memcmp(now_caps, caps, 16) == 0;
}

static int elevate(void)
{
    static const uint8_t caps[16] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    pid_t me = getpid();

    if (!g_have_orig) {
        g_orig_authid = kernel_get_ucred_authid(me);
        if (!g_orig_authid || kernel_get_ucred_caps(me, g_orig_caps) != 0) {
            /* The SDK does not know this firmware's kernel. A loader that
             * already runs payloads as root (kstuff on newer firmwares) may
             * make raising unnecessary: try without, and let the virtual
             * pad calls say. */
            if (getuid() == 0 || geteuid() == 0) {
                log_line("vpad: cannot read our credentials, but the process is root: trying without raising them");
                g_raised = 0;
                return 1;
            }
            log_line("vpad: cannot read our credentials");
            return 0;
        }
        g_have_orig = 1;
    }
    if (!set_creds(0x3800000000010003ull, caps)) {
        log_line("vpad: credentials not raised; putting the old ones back");
        set_creds(g_orig_authid, g_orig_caps);
        g_raised = 0;
        if (getuid() == 0 || geteuid() == 0) {
            log_line("vpad: the process is root, trying with what it has");
            return 1;
        }
        return 0;
    }
    g_raised = 1;
    return 1;
}

int vpad_creds_restore(void)
{
    if (!g_have_orig || !g_raised) return 1;
    if (!set_creds(g_orig_authid, g_orig_caps)) {
        log_line("vpad: could not put the original credentials back");
        return 0;
    }
    g_raised = 0;
    log_line("vpad: original credentials restored");
    return 1;
}

int vpad_creds_raise(void)
{
    return g_raised || elevate();
}

int vpad_init(void)
{
    static const char *const mbus[] = {
        "/system/common/lib/libSceMbus.sprx", "libSceMbus.sprx",
    };
    void *lib = NULL;
    int32_t r;
    size_t i;

    if (!elevate()) return 0;
    if ((r = sceUserServiceInitialize(NULL)) != 0)
        log_line("vpad: sceUserServiceInitialize %#x (already initialised is fine)", (unsigned)r);

    /* libScePad imports from libSceMbus: load it before any scePad call,
     * or scePadInit can fault on an unresolved import. */
    for (i = 0; i < sizeof mbus / sizeof mbus[0] && !lib; i++)
        lib = dlopen(mbus[i], RTLD_NOW | RTLD_GLOBAL);
    if (lib) g_bind = (mbus_bind_fn)dlsym(lib, "sceMbusBindDeviceWithUserId");
    if (!g_bind) {
        log_line("vpad: libSceMbus not available, refusing to touch libScePad");
        return 0;
    }
    /* scePadInit first. scePadSetProcessPrivilege before it answers
     * 0x80920005, "not initialised" (found on the first run on a console,
     * firmware 10.01). The
     * privilege call is not checked as a precondition: the virtual device
     * calls report their own errors. */
    if ((r = scePadInit()) != 0) {
        log_line("vpad: scePadInit failed: %#x", (unsigned)r);
        return 0;
    }
    if ((r = scePadSetProcessPrivilege(1)) != 0)
        log_line("vpad: scePadSetProcessPrivilege answered %#x, carrying on", (unsigned)r);
    g_user = main_user();
    log_line("vpad: main user %#x", (unsigned)g_user);
    g_ready = 1;
    return 1;
}

/* ---- virtual pads ----------------------------------------------------------- */

int vpad_add(int slot)
{
    if (!g_ready || slot < 0 || slot >= VPAD_MAX_SLOTS) return 0;
    if (g_slot[slot].st != VP_FREE && g_slot[slot].st != VP_FAILED) return 0;
    memset(&g_slot[slot], 0, sizeof g_slot[slot]);
    g_slot[slot].st = VP_QUEUED;
    g_slot[slot].handle = -1;
    return 1;
}

static void start_identification(int slot)
{
    int32_t param[8];
    int32_t r;
    int i;

    g_user = main_user();                       /* who is signed in now */
    if (g_user == -1) {
        log_line("slot %d: nobody signed in, no virtual pad (tried again in %d s)", slot, T_USER_RETRY / 1000);
        g_slot[slot].st = VP_FAILED;
        g_slot[slot].why = VF_USER;
        g_slot[slot].t_fail = now_ms();
        return;
    }
    if (!vpad_creds_raise()) {
        g_slot[slot].st = VP_FAILED;
        g_slot[slot].why = VF_CREDS;
        return;
    }
    g_log_fd = log_source_open();
    if (g_log_fd < 0) {
        g_slot[slot].st = VP_FAILED;
        g_slot[slot].why = VF_LOG;
        return;
    }
    log_source_read();                  /* what came before is not ours */
    g_matches = 0;
    g_match_dev = 0;

    /* The parameter block as the PS5 accepts it: size 32, user 1,
     * the rest marked so that a call writing into it shows in the log. */
    for (i = 0; i < 8; i++) param[i] = (int32_t)0xDEADBEEF;
    param[0] = (int32_t)sizeof param;
    param[1] = 1;
    r = scePadVirtualDeviceAddDevice(param, VIRTUAL_DEVICE_DUALSENSE);
    log_line("slot %d: AddDevice %#x, block %08x %08x %08x %08x %08x %08x", slot, (unsigned)r,
             (unsigned)param[2], (unsigned)param[3], (unsigned)param[4], (unsigned)param[5],
             (unsigned)param[6], (unsigned)param[7]);
    g_pending = slot;
    g_t_add = now_ms();
    g_slot[slot].st = VP_PENDING;
}

/* Binds the slot's device to the chosen user; if the console says that user
 * is not signed in (seen after switching profiles), to every other signed-in
 * user in turn. Returns the last bind result. */
static int32_t try_bind(vslot *v, int slot)
{
    int32_t r;

    if (g_user == -1) g_user = main_user();
    if (g_user == -1) return (int32_t)BIND_USER_NOT_LOGIN;
    r = g_bind(v->dev, g_user);
    log_line("slot %d: device %#llx bound to user %#x: %#x", slot, (unsigned long long)v->dev,
             (unsigned)g_user, (unsigned)r);
    if ((uint32_t)r == BIND_USER_NOT_LOGIN) {
        int32_t login[4] = { -1, -1, -1, -1 };
        int i;
        if (sceUserServiceGetLoginUserIdList(login) != 0) login[0] = login[1] = login[2] = login[3] = -1;
        for (i = 0; i < 4 && r != 0; i++) {
            if (login[i] == -1 || login[i] == 0 || login[i] == g_user) continue;
            r = g_bind(v->dev, login[i]);
            log_line("slot %d: device %#llx bound to user %#x instead: %#x", slot,
                     (unsigned long long)v->dev, (unsigned)login[i], (unsigned)r);
            if (r == 0) g_user = login[i];
        }
    }
    return r;
}

static void finish_identification(long now)
{
    vslot *v = &g_slot[g_pending];
    int slot = g_pending;
    int32_t r;

    if (g_matches == 0 && now - g_t_add < T_IDENTIFY) return;
    if (g_matches == 1 && now - g_t_match < T_AMBIGUITY && now - g_t_add < T_IDENTIFY) return;

    g_pending = -1;
    log_source_close();
    if (g_matches != 1) {
        /* Unknown or ambiguous: nothing is bound, inserted into or deleted. */
        log_line("slot %d: virtual pad not identified (%d candidates), left alone", slot, g_matches);
        v->st = VP_FAILED;
        v->why = VF_IDENT;
        return;
    }
    v->dev = g_match_dev;
    v->handle = (int32_t)(g_match_dev & 0xFFFFFFFFu);
    if (v->remove_when_found) {
        scePadVirtualDeviceDeleteDevice(v->handle);
        log_line("slot %d: device %#llx removed: its pad left meanwhile", slot,
                 (unsigned long long)v->dev);
        memset(v, 0, sizeof *v);
        return;
    }
    r = try_bind(v, slot);
    v->user = r == 0 ? g_user : -1;
    if ((uint32_t)r == BIND_USER_NOT_LOGIN) {
        /* Left alive and fed: the console's own prompt may assign it, and
         * binding is tried again (vpad_poll, or the menu's Retry). */
        v->st = VP_UNBOUND;
        v->why = VF_USER;
        v->t_fail = now;
        log_line("slot %d: no signed-in user takes the pad; left for the console's prompt, tried again every %d s",
                 slot, T_USER_RETRY / 1000);
        return;
    }
    if (r != 0) {
        scePadVirtualDeviceDeleteDevice(v->handle);     /* ours, identified */
        v->st = VP_FAILED;
        v->why = VF_REFUSED;
        v->t_fail = now;
        return;
    }
    v->st = VP_READY;
}

void vpad_poll(long now)
{
    int i;

    if (!g_ready) return;
    ps_tick(now);
    probe_tick(now);
    if (g_pending >= 0) {
        log_source_read();
        finish_identification(now);
        return;
    }
    for (i = 0; i < VPAD_MAX_SLOTS; i++) {
        vslot *v = &g_slot[i];
        if (v->st == VP_UNBOUND && now - v->t_fail >= T_USER_RETRY) {
            g_user = -1;                /* a user may have signed in since */
            v->t_fail = now;
            if (try_bind(v, i) == 0) v->st = VP_READY;
        }
        if (v->st == VP_FAILED && v->why == VF_USER && now - v->t_fail >= T_USER_RETRY) {
            v->st = VP_QUEUED;
            v->handle = -1;
        }
        if (v->st == VP_QUEUED) {
            start_identification(i);
            return;
        }
    }
}

const char *vpad_status(int slot)
{
    const vslot *v;

    if (slot < 0 || slot >= VPAD_MAX_SLOTS) return "";
    v = &g_slot[slot];
    switch (v->st) {
    case VP_QUEUED:
    case VP_PENDING: return "creating";
    case VP_READY:   return "ready";
    case VP_UNBOUND: return "waiting for a user";
    case VP_FAILED:
        switch (v->why) {
        case VF_USER:    return "no user signed in";
        case VF_IDENT:   return "not identified";
        case VF_CREDS:   return "no privileges";
        case VF_LOG:     return "no kernel log";
        default:         return "refused";
        }
    default:         return "";
    }
}

int vpad_live(int slot)
{
    return slot >= 0 && slot < VPAD_MAX_SLOTS &&
           (g_slot[slot].st == VP_READY || g_slot[slot].st == VP_UNBOUND);
}

/* ---- the remote-setting probe -------------------------------------------------
 *
 * scePadVirtualDeviceGetRemoteSetting is exported by libScePad and not
 * documented anywhere public. Its arguments are unknown, so it is called
 * with the device handle first and the same large, zeroed buffer in every
 * other argument register: whichever of them is the output pointer gets a
 * valid 16 KB, and an argument that is really a number just gets a strange
 * value. Anything it writes, and its return code, go to the log. */
#define PROBE_BUF   16384
#define PROBE_SHOW    96        /* bytes of the structure shown in the log */
#define T_PROBE_CALL   50
#define T_PROBE_SELF 4000       /* between the vibration/light bar settings the probe makes itself */
typedef int32_t (*remote_setting_fn)(int32_t handle, void *a, void *b, void *c, void *d);
typedef int32_t (*set_vibration_fn)(int32_t handle, const void *param);
typedef int32_t (*set_lightbar_fn)(int32_t handle, const void *param);
static remote_setting_fn g_remote_setting;
static set_vibration_fn g_set_vibration;
static set_lightbar_fn g_set_lightbar;
static int g_remote_looked;
static struct {
    long until;                 /* probing until, 0: not */
    long t_last, t_self;
    int32_t rc_last;
    int calls, changes, bytes_max;
    int32_t pad_handle;         /* the virtual pad opened as a pad, for the self-test; -1 none */
    int pad_opened;             /* by us (closed at the end) */
    int self_step;
    char status[160];
    uint8_t last[PROBE_BUF];
} g_probe[VPAD_MAX_SLOTS];
static uint8_t g_probe_buf[PROBE_BUF];

static int probe_resolve(void)
{
    if (!g_remote_looked) {
        g_remote_looked = 1;
        g_remote_setting = (remote_setting_fn)dlsym(RTLD_DEFAULT, "scePadVirtualDeviceGetRemoteSetting");
        g_set_vibration = (set_vibration_fn)dlsym(RTLD_DEFAULT, "scePadSetVibration");
        g_set_lightbar = (set_lightbar_fn)dlsym(RTLD_DEFAULT, "scePadSetLightBar");
        log_line("probe: scePadVirtualDeviceGetRemoteSetting %s, scePadSetVibration %s, scePadSetLightBar %s",
                 g_remote_setting ? "found" : "not on this firmware", g_set_vibration ? "found" : "missing",
                 g_set_lightbar ? "found" : "missing");
    }
    return g_remote_setting != NULL;
}

/* The self-test: the virtual pad opened as an ordinary pad of its user, so
 * the probe can set vibration and the light bar on it the way a game does
 * and watch the remote setting answer. */
static void probe_open_pad(int slot)
{
    vslot *v = &g_slot[slot];
    int32_t h;

    g_probe[slot].pad_handle = -1;
    if (v->user == -1) {
        log_line("slot %d: probe: the virtual pad is bound to no user, no self-test", slot);
        return;
    }
    h = scePadGetHandle(v->user, 0, 0);
    if (h < 0) {
        h = scePadOpen(v->user, 0, 0, NULL);
        if (h >= 0) g_probe[slot].pad_opened = 1;
    }
    log_line("slot %d: probe: pad handle for user %#x: %#x%s", slot, (unsigned)v->user, (unsigned)h,
             h >= 0 ? (g_probe[slot].pad_opened ? " (opened)" : " (existing)") : " (none: no self-test)");
    g_probe[slot].pad_handle = h;
}

int vpad_probe_start(int slot, int seconds)
{
    if (!vpad_live(slot) || !probe_resolve()) return 0;
    memset(&g_probe[slot], 0, sizeof g_probe[slot]);
    g_probe[slot].until = now_ms() + (long)seconds * 1000;
    snprintf(g_probe[slot].status, sizeof g_probe[slot].status, "probing for %d s: play something that rumbles", seconds);
    log_line("slot %d: probing the remote setting of device %#llx (handle %#x) for %d s", slot,
             (unsigned long long)g_slot[slot].dev, (unsigned)g_slot[slot].handle, seconds);
    probe_open_pad(slot);
    return 1;
}

/* Every T_PROBE_SELF: a different vibration or light bar on the pad. */
static void probe_self_step(int slot, long now)
{
    static const struct { uint8_t big, small, r, g, b; const char *what; } steps[] = {
        { 200, 0,   0, 0, 0,   "vibration big 200 small 0" },
        { 0,   0,   0, 0, 0,   "vibration off" },
        { 0,   200, 0, 0, 0,   "vibration big 0 small 200" },
        { 0,   0,   0, 0, 0,   "vibration off" },
        { 0,   0,   255, 0, 0, "light bar red" },
        { 0,   0,   0, 0, 255, "light bar blue" },
    };
    int32_t h = g_probe[slot].pad_handle, rc;
    int k;

    if (h < 0 || now - g_probe[slot].t_self < T_PROBE_SELF) return;
    g_probe[slot].t_self = now;
    k = g_probe[slot].self_step++ % (int)(sizeof steps / sizeof steps[0]);
    if (k < 4) {
        uint8_t param[8] = { steps[k].big, steps[k].small, 0, 0, 0, 0, 0, 0 };
        rc = g_set_vibration ? g_set_vibration(h, param) : (int32_t)0xFFFFFFFF;
    } else {
        uint8_t param[8] = { steps[k].r, steps[k].g, steps[k].b, 0, 0, 0, 0, 0 };
        rc = g_set_lightbar ? g_set_lightbar(h, param) : (int32_t)0xFFFFFFFF;
    }
    log_line("slot %d: probe self-test: %s -> %#x", slot, steps[k].what, (unsigned)rc);
}

const char *vpad_probe_status(int slot)
{
    if (slot < 0 || slot >= VPAD_MAX_SLOTS) return "";
    return g_probe[slot].status;
}

static void probe_tick(long now)
{
    int i, k, n, first;

    for (i = 0; i < VPAD_MAX_SLOTS; i++) {
        int32_t rc;
        if (!g_probe[i].until) continue;
        if (now >= g_probe[i].until || !vpad_live(i)) {
            if (g_probe[i].pad_handle >= 0 && g_set_vibration) {
                uint8_t off[8] = { 0 };
                g_set_vibration(g_probe[i].pad_handle, off);
            }
            if (g_probe[i].pad_opened && g_probe[i].pad_handle >= 0) scePadClose(g_probe[i].pad_handle);
            g_probe[i].pad_handle = -1;
            snprintf(g_probe[i].status, sizeof g_probe[i].status,
                     "probe done: %d calls, last result %#x, %d changes, %d bytes at most; see the log",
                     g_probe[i].calls, (unsigned)g_probe[i].rc_last, g_probe[i].changes, g_probe[i].bytes_max);
            log_line("slot %d: %s", i, g_probe[i].status);
            g_probe[i].until = 0;
            continue;
        }
        probe_self_step(i, now);
        if (now - g_probe[i].t_last < T_PROBE_CALL) continue;
        g_probe[i].t_last = now;
        memset(g_probe_buf, 0, sizeof g_probe_buf);
        rc = g_remote_setting(g_slot[i].handle, g_probe_buf, g_probe_buf, g_probe_buf, g_probe_buf);
        g_probe[i].calls++;
        if (g_probe[i].calls == 1 || rc != g_probe[i].rc_last)
            log_line("slot %d: probe call %d returned %#x", i, g_probe[i].calls, (unsigned)rc);
        g_probe[i].rc_last = rc;
        for (n = PROBE_BUF; n > 0 && g_probe_buf[n - 1] == 0; n--) ;      /* written length, roughly */
        if (n > g_probe[i].bytes_max) g_probe[i].bytes_max = n;
        /* Byte 0 is a "new since you last asked" flag: ignored for the
         * comparison, so only real changes are logged. */
        if (memcmp(g_probe_buf + 1, g_probe[i].last + 1, PROBE_SHOW - 1) != 0 || g_probe[i].changes == 0) {
            char hex[3 * PROBE_SHOW + 4], diff[200];
            size_t dn = 0;
            int m = n < PROBE_SHOW ? n : PROBE_SHOW;
            for (k = 0; k < m; k++) snprintf(hex + 3 * k, 4, "%02x ", g_probe_buf[k]);
            hex[3 * m] = '\0';
            if (n > PROBE_SHOW) strcat(hex, "...");
            diff[0] = '\0';
            for (k = 1; k < PROBE_SHOW && dn < sizeof diff - 24; k++)
                if (g_probe_buf[k] != g_probe[i].last[k])
                    dn += (size_t)snprintf(diff + dn, sizeof diff - dn, " [%d] %02x->%02x", k,
                                           g_probe[i].last[k], g_probe_buf[k]);
            g_probe[i].changes++;
            first = g_probe[i].changes == 1;
            log_line("slot %d: probe data %s(%d bytes): %s", i, first ? "" : "changed ", n, n ? hex : "(all zero)");
            if (!first && diff[0]) log_line("slot %d: probe diff:%s", i, diff);
            memcpy(g_probe[i].last, g_probe_buf, PROBE_BUF);
            snprintf(g_probe[i].status, sizeof g_probe[i].status,
                     "probing: result %#x, %d bytes, %d changes so far", (unsigned)rc, n, g_probe[i].changes);
        }
    }
}

int vpad_reassign(int slot)
{
    if (!g_ready || slot < 0 || slot >= VPAD_MAX_SLOTS || g_slot[slot].st == VP_FREE) return 0;
    vpad_remove(slot);
    g_user = -1;                        /* pick the user afresh */
    return vpad_add(slot);
}

int vpad_users(vpad_user *out, int max)
{
    int32_t login[4] = { -1, -1, -1, -1 }, init = -1;
    int i, n = 0;

    if (sceUserServiceGetLoginUserIdList(login) != 0) return 0;
    if (sceUserServiceGetInitialUser(&init) != 0) init = -1;
    for (i = 0; i < 4 && n < max; i++) {
        if (login[i] == -1 || login[i] == 0) continue;
        out[n].id = login[i];
        out[n].initial = login[i] == init;
        if (sceUserServiceGetUserName(login[i], out[n].name, sizeof out[n].name) != 0)
            snprintf(out[n].name, sizeof out[n].name, "User %x", (unsigned)login[i]);
        n++;
    }
    return n;
}

int vpad_rebind(int slot)
{
    vslot *v;

    if (!g_ready || slot < 0 || slot >= VPAD_MAX_SLOTS) return 0;
    v = &g_slot[slot];
    if (v->st == VP_UNBOUND) {
        g_user = -1;
        v->t_fail = now_ms();
        log_line("slot %d: binding retried from the menu", slot);
        if (try_bind(v, slot) == 0) {
            v->st = VP_READY;
            return 1;
        }
        return 2;
    }
    if (v->st == VP_FAILED) {
        log_line("slot %d: virtual pad made again from the menu", slot);
        memset(v, 0, sizeof *v);
        v->st = VP_QUEUED;
        v->handle = -1;
        return 2;
    }
    return 0;
}

/* One frame to the virtual pad: the pad's state, with PS added while the menu
 * is "holding" it. */
static void send_frame(int slot, const pad_state *st)
{
    ScePadData d;
    struct timespec ts;
    int i;

    if (!vpad_live(slot)) return;
    memset(&d, 0, sizeof d);
    d.buttons = st->buttons | (g_slot[slot].ps_until ? PAD_PS : 0);
    d.lx = st->lx;
    d.ly = st->ly;
    d.rx = st->rx;
    d.ry = st->ry;
    d.l2 = st->l2;
    d.r2 = st->r2;
    d.orientation[3] = 1.0f;
    if (st->has_motion) {
        for (i = 0; i < 3; i++) {
            d.acceleration[i] = st->accel[i] / ACCEL_PER_G;
            d.angularVelocity[i] = st->gyro[i] / GYRO_PER_DEG_S * DEG_TO_RAD;
        }
    } else {
        d.acceleration[1] = -1.0f;      /* at rest, held level */
    }
    if (st->has_touch) {
        for (i = 0; i < 2; i++) {
            ScePadTouch *t = &d.touchData.touch[d.touchData.touchNum];
            if (!st->touch[i].down) continue;
            t->x = st->touch[i].x;
            t->y = st->touch[i].y;
            t->id = (uint8_t)i;
            d.touchData.touchNum++;
        }
    }
    d.connected = 1;
    d.connectedCount = 1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d.timestamp = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;

    if (scePadVirtualDeviceInsertData(g_slot[slot].handle, &d) != 0) {
        /* Stop feeding a device that refuses: it may not be ours any more. */
        log_line("slot %d: the virtual pad refused input, stopped feeding it", slot);
        g_slot[slot].st = VP_FAILED;
    }
}

void vpad_update(int slot, const pad_state *st)
{
    if (!vpad_live(slot)) return;
    g_slot[slot].last = *st;
    send_frame(slot, st);
}

/* Holds PS on a virtual pad for `ms`, as pressing the PS button on the pad
 * would: the console hands the pad over to its user, or opens its menu. */
int vpad_press_ps(int slot, int ms)
{
    if (!vpad_live(slot) || ms <= 0) return 0;
    g_slot[slot].ps_until = now_ms() + ms;
    g_slot[slot].ps_active = 1;
    log_line("slot %d: PS pressed from the menu (%d ms)", slot, ms);
    send_frame(slot, &g_slot[slot].last);
    return 1;
}

/* Keeps the PS frames going (the pad may be silent) and ends the press. */
static void ps_tick(long now)
{
    int i;

    for (i = 0; i < VPAD_MAX_SLOTS; i++) {
        vslot *v = &g_slot[i];
        if (!v->ps_active || !vpad_live(i)) continue;
        if (v->ps_until && now >= v->ps_until) v->ps_until = 0;       /* released */
        send_frame(i, &v->last);
        if (!v->ps_until) v->ps_active = 0;
    }
}

/* Removes only what this instance created and identified. */
void vpad_remove(int slot)
{
    vslot *v;

    if (slot < 0 || slot >= VPAD_MAX_SLOTS) return;
    v = &g_slot[slot];
    switch (v->st) {
    case VP_READY:
    case VP_UNBOUND:
        scePadVirtualDeviceDeleteDevice(v->handle);
        log_line("slot %d: virtual DualSense removed", slot);
        memset(v, 0, sizeof *v);
        break;
    case VP_PENDING:
        v->remove_when_found = 1;       /* finish_identification deletes it */
        break;
    default:
        memset(v, 0, sizeof *v);
        break;
    }
}

static int32_t g_phys_err;
static int32_t g_phys_handle = -1;
static int g_phys_ours;                 /* we opened it, so we close it */
static long g_phys_retry_at;
static int32_t g_open_logged = 1;
static long g_user_retry_at;

int32_t vpad_physical_error(void) { return g_phys_err; }

/* The handle for the user's pad in this process. A payload has none until
 * it opens the pad, which is what any application does (the system's own
 * UI and the running game each hold the pad too). A refusal is logged and
 * tried again at most every 5 seconds. */
static int32_t physical_handle(void)
{
    int32_t h;

    if (g_phys_handle >= 0) return g_phys_handle;
    h = scePadGetHandle(g_user, 0, 0);
    if (h < 0 && now_ms() >= g_phys_retry_at) {
        g_phys_retry_at = now_ms() + 5000;
        h = scePadOpen(g_user, 0, 0, NULL);
        if (h != g_open_logged) {                   /* only when the answer changes */
            log_line("vpad: scePadOpen of the user's pad (user %#x): %#x", (unsigned)g_user, (unsigned)h);
            g_open_logged = h;
        }
        if (h >= 0) g_phys_ours = 1;
        /* The user may sign in or out meanwhile: look again. */
        if (h == (int32_t)0x809B0081) g_user = main_user();
    }
    if (h >= 0) g_phys_handle = h;
    return h;
}

void vpad_close_physical(void)
{
    if (g_phys_handle >= 0 && g_phys_ours) scePadClose(g_phys_handle);
    g_phys_handle = -1;
    g_phys_ours = 0;
}

int vpad_read_physical(uint32_t *buttons)
{
    ScePadData d;
    int32_t h, r;

    /* Only reads the user's own pad, through the handle libScePad gives for
     * it: never opens, takes over or disconnects it. */
    if (!g_ready) {
        g_phys_err = (int32_t)0xFFFF0001;               /* not ready */
        return 0;
    }
    if (g_user == -1) {                 /* nobody was signed in; look again now and then */
        if (now_ms() >= g_user_retry_at) {
            g_user_retry_at = now_ms() + 5000;
            g_user = main_user();
        }
        if (g_user == -1) {
            g_phys_err = (int32_t)0xFFFF0004;           /* nobody signed in */
            return 0;
        }
    }
    h = physical_handle();
    if (h < 0) {
        g_phys_err = h;
        return 0;
    }
    r = scePadReadState(h, &d);
    if (r != 0) {
        g_phys_err = r;
        if (g_phys_ours) scePadClose(h);
        g_phys_handle = -1;             /* start again from the handle */
        g_phys_ours = 0;
        return 0;
    }
    if (!d.connected) {
        g_phys_err = (int32_t)0xFFFF0002;               /* no pad connected */
        return 0;
    }
    g_phys_err = 0;
    /* The system takes the pad's input while it has focus: that reads as the
     * 'intercepted' bit, and then the buttons are not ours to use. */
    if (d.buttons & 0x80000000u) {
        g_phys_err = (int32_t)0xFFFF0003;               /* intercepted */
        return 0;
    }
    *buttons = d.buttons;
    return 1;
}
