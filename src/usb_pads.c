#include "usb_pads.h"
#include "gip.h"
#include "log.h"
#include "profiles.h"
#include "usb_desc.h"
#include "usb_quirks.h"
#include "util.h"
#include "xinput.h"

#include <sys/ioctl.h>
#include <sys/types.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>              /* USB_ERR_* */
#include <dev/usb/usb_ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define XFER_BUF 256
#define EP_IN    0
#define EP_OUT   1
#define EP_MAX   2

#define T_RESCAN        2500    /* look for new pads this often (ms) */
#define T_INIT_STEP      250    /* an init message not completed by then is dropped */
#define T_PROF_STEP       60    /* between a profile's wake-up reports */
#define T_PROF_RETRY     800    /* before repeating an unanswered wake-up sequence */
#define PROF_ROUNDS        5
#define T_SILENT        3000    /* a streaming pad quiet this long: re-initialise */
#define T_ERRORS        2000    /* real transfer errors, without a break, this long: rebuild, then gone */
#define T_IGNORE      600000    /* a node that is not a game pad is left alone this long */
#define REINITS_BEFORE_REBUILD 3
#define STREAM_RATE       20    /* reports in a second that make a pad "streaming" */
#define DESC_MAX        1024

/* Most pads send a report only when something changes, so silence means
 * nothing: an idle pad is not a stalled one (treating it as one
 * re-initialised it under the player's hands and froze it for seconds).
 * Only a pad seen streaming reports is re-initialised when it goes quiet.
 * A pad that is gone shows up as transfer errors, or its node vanishing. */

/* An interface and its interrupt endpoints. */
typedef struct {
    int iface, ep_in, ep_out, mps_in;
    int hid_desc_len;           /* HID: the report descriptor's length, 0 unknown */
} layout;

typedef struct {
    int used;
    int fd;
    char node[32];
    unsigned vid, pid;
    layout lay;
    int kind;                   /* WIRED_* */
    int slot, state;
    unsigned char seq;

    struct usb_fs_endpoint eps[EP_MAX];
    void *bufptr[EP_MAX][1];
    uint32_t buflen[EP_MAX][1];
    unsigned char data[EP_MAX][XFER_BUF];
    int pending[EP_MAX];
    long t_out;                 /* when the OUT transfer was queued */

    /* Bringing it up: the kind's own messages, then a profile's. */
    int init_step;              /* next start message, or past the end */
    int prof_step, prof_rounds, prof_done, full;
    long t_prof;

    /* GIP */
    int guide;                  /* Guide held */
    int has_batt, batt_pct, batt_charging;

    /* HID */
    int model;                  /* USBPAD_* */
    const pad_profile *prof;
    union { unsigned char bytes[PROFILE_CTX_MAX]; uint64_t align; } pctx;

    /* XInput receiver */
    int present;

    int reinits;                /* fruitless re-initialisations in a row */
    long t_report, t_keepalive, t_reinit, reports;
    int streams;                /* sends reports continuously */
    long t_window, in_window;   /* report rate measurement */
    long t_err_first;           /* transfers failing since, 0: fine */
    int rebuilt;                /* rebuilt once for these errors */
    int gone;                   /* the device is detached (ioctl says so) */
    int n_err_logged;
    int announced;              /* on_connect called */
    pad_output out;             /* lights and rumble as last asked */
    pad_state st;
} wired_pad;

static wired_pad g_pad[WIRED_MAX_PADS];
static host_events g_ev;
static int g_first_slot = -1;
static char g_skip[32];
static char g_map_dir[256];
static long g_t_scan;
static int g_keepalive_ms;      /* re-send a GIP pad's init sequence this often, 0: never */
static struct { char node[32]; long until; } g_ignore[16];

static const char *kind_word(int kind)
{
    static const char *const w[] = { "gip", "xinput", "receiver", "hid" };
    return kind >= 0 && kind <= 3 ? w[kind] : "?";
}

/* ---- descriptors ----------------------------------------------------------- */

/* Walks a configuration descriptor for the first interface of the given
 * class (and subclass/protocol when not -1), and its interrupt endpoints.
 * Returns 1 if found with an interrupt IN endpoint. */
static int find_iface(const unsigned char *cfg, int len, int cls, int sub, int proto, layout *out)
{
    int i = 0, in = 0;

    memset(out, 0, sizeof *out);
    out->iface = out->ep_in = out->ep_out = -1;
    while (i + 2 <= len) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen == 0 || i + blen > len) break;
        if (btype == 0x04 && blen >= 9) {                       /* interface */
            int match = cfg[i + 5] == cls && (sub < 0 || cfg[i + 6] == sub) &&
                        (proto < 0 || cfg[i + 7] == proto) && cfg[i + 3] == 0;    /* alternate 0 */
            if (match && out->iface < 0) {
                out->iface = cfg[i + 2];
                in = 1;
            } else {
                in = in && out->iface == cfg[i + 2];
            }
        } else if (btype == 0x21 && blen >= 9 && in) {          /* HID descriptor */
            out->hid_desc_len = cfg[i + 7] | cfg[i + 8] << 8;
        } else if (btype == 0x05 && blen >= 7 && in) {          /* endpoint */
            int addr = cfg[i + 2], attr = cfg[i + 3], mps = cfg[i + 4] | cfg[i + 5] << 8;
            if ((attr & 3) == 3) {
                if ((addr & 0x80) && out->ep_in < 0) {
                    out->ep_in = addr;
                    out->mps_in = mps;
                } else if (!(addr & 0x80) && out->ep_out < 0) {
                    out->ep_out = addr;
                }
            }
        }
        i += blen;
    }
    return out->iface >= 0 && out->ep_in >= 0;
}

/* A control request through the device node. Returns the bytes moved, -1. */
static int control(int fd, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                   void *data, int len)
{
    struct usb_ctl_request r;

    memset(&r, 0, sizeof r);
    r.ucr_request.bmRequestType = type;
    r.ucr_request.bRequest = req;
    USETW(r.ucr_request.wValue, value);
    USETW(r.ucr_request.wIndex, index);
    USETW(r.ucr_request.wLength, (uint16_t)len);
    r.ucr_data = data;
    if (ioctl(fd, USB_DO_REQUEST, &r) != 0) return -1;
    return (int)r.ucr_actlen;
}

/* ---- transfers ---------------------------------------------------------- */

static int fs_setup(wired_pad *p)
{
    struct usb_fs_init init;
    struct usb_fs_open op;
    int i;

    memset(p->eps, 0, sizeof p->eps);
    memset(p->pending, 0, sizeof p->pending);
    for (i = 0; i < EP_MAX; i++) {
        p->bufptr[i][0] = p->data[i];
        p->buflen[i][0] = XFER_BUF;
        p->eps[i].ppBuffer = p->bufptr[i];
        p->eps[i].pLength = p->buflen[i];
        p->eps[i].nFrames = 1;
        p->eps[i].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        p->eps[i].timeout = 1000;
    }
    memset(&init, 0, sizeof init);
    init.pEndpoints = p->eps;
    init.ep_index_max = EP_MAX;
    if (ioctl(p->fd, USB_FS_INIT, &init) != 0) return 0;

    memset(&op, 0, sizeof op);
    op.max_bufsize = XFER_BUF;
    op.max_frames = 1;
    op.ep_index = EP_IN;
    op.ep_no = (uint8_t)p->lay.ep_in;
    if (ioctl(p->fd, USB_FS_OPEN, &op) != 0) return 0;
    if (p->lay.ep_out >= 0) {
        memset(&op, 0, sizeof op);
        op.max_bufsize = XFER_BUF;
        op.max_frames = 1;
        op.ep_index = EP_OUT;
        op.ep_no = (uint8_t)p->lay.ep_out;
        ioctl(p->fd, USB_FS_OPEN, &op);     /* input alone is still useful */
    }
    return 1;
}

/* An ioctl failing this way means the device is detached (unplugged, or
 * the node reused), not that a transfer was refused. */
static int detached_errno(void)
{
    return errno == ENXIO || errno == ENODEV || errno == EBADF || errno == ESHUTDOWN;
}

/* Queues a transfer unless one is already outstanding on that endpoint. */
static int fs_start(wired_pad *p, int index, int length)
{
    struct usb_fs_start start;

    if (p->pending[index]) return 1;
    p->buflen[index][0] = (uint32_t)length;
    p->eps[index].nFrames = 1;
    p->eps[index].aFrames = 0;
    p->eps[index].status = 0;
    memset(&start, 0, sizeof start);
    start.ep_index = (uint8_t)index;
    if (ioctl(p->fd, USB_FS_START, &start) != 0) {
        if (detached_errno()) p->gone = 1;
        return 0;                       /* refused for now: tried again next poll */
    }
    p->pending[index] = 1;
    if (index == EP_OUT) p->t_out = now_ms();
    return 1;
}

/* Collects one completion, for whichever endpoint finished. Returns the
 * length of an input report; 0 for nothing (including an idle endpoint's
 * read timing out, which is the normal state of a pad that reports only on
 * change, and a cancelled read); -1 on a real error of the IN transfer. */
static int fs_poll(wired_pad *p)
{
    struct usb_fs_complete comp;
    int index, status;

    memset(&comp, 0, sizeof comp);
    if (ioctl(p->fd, USB_FS_COMPLETE, &comp) != 0) {
        if (detached_errno()) p->gone = 1;
        return 0;
    }
    index = (int)comp.ep_index;
    if (index < 0 || index >= EP_MAX) return 0;
    p->pending[index] = 0;
    if (index != EP_IN) return 0;
    status = (int)p->eps[EP_IN].status;
    if (status == USB_ERR_TIMEOUT || status == USB_ERR_CANCELLED) return 0;
    if (status != 0) {
        if (p->n_err_logged < 3) {
            p->n_err_logged++;
            log_line("usb pad %s: read failed with USB error %d", p->node, status);
        }
        return -1;
    }
    if (p->eps[EP_IN].aFrames == 0) return 0;
    return (int)p->buflen[EP_IN][0];
}

/* Sends on the interrupt OUT endpoint; a HID pad without one gets the
 * report as a SET_REPORT control request instead. */
static int pad_send(wired_pad *p, const unsigned char *msg, int n)
{
    if (n <= 0 || n > XFER_BUF) return 0;
    if (p->lay.ep_out < 0) {
        if (p->kind != WIRED_HID) return 0;
        return control(p->fd, 0x21, 0x09, (uint16_t)(0x0200 | msg[0]), (uint16_t)p->lay.iface,
                       (void *)msg, n) >= 0;
    }
    if (p->pending[EP_OUT]) {
        /* A write the controller never completed: give it up after a while,
         * or nothing could ever be sent again. */
        if (now_ms() - p->t_out < T_INIT_STEP) return 0;
        p->pending[EP_OUT] = 0;
    }
    memcpy(p->data[EP_OUT], msg, (size_t)n);
    return fs_start(p, EP_OUT, n);
}

/* ---- what each kind sends ----------------------------------------------- */

/* The output report (rumble, lights) for the pad as it is now. */
static int build_output(wired_pad *p, unsigned char *out, int max)
{
    switch (p->kind) {
    case WIRED_GIP:
        return gip_build_rumble(p->seq++, p->out.strong, p->out.weak, out);
    case WIRED_XINPUT:
        return xinput_build_rumble(XINPUT_WIRED, p->out.strong, p->out.weak, out);
    case WIRED_X360W:
        return xinput_build_rumble(XINPUT_RECEIVER, p->out.strong, p->out.weak, out);
    case WIRED_HID: {
        int n = usb_quirks_output(p->model, &p->out, out, max);
        if (n > 0) return n;
        if (p->prof && p->prof->build_output) return p->prof->build_output(p->pctx.bytes, &p->out, out, max);
        return 0;
    }
    default:
        return 0;
    }
}

/* Step `step` of the kind's own start-up messages; 0 past the end. */
static int build_start(wired_pad *p, int step, unsigned char *out, int max)
{
    switch (p->kind) {
    case WIRED_GIP:
        return gip_build_init(step, p->seq++, out);
    case WIRED_XINPUT:
        return step == 0 ? xinput_build_led(XINPUT_WIRED, p->slot - g_first_slot + 1, out) : 0;
    case WIRED_X360W:
        if (step == 0) return xinput_build_presence_query(out);
        if (step == 1) return xinput_build_led(XINPUT_RECEIVER, p->slot - g_first_slot + 1, out);
        return 0;
    case WIRED_HID: {
        int n = usb_quirks_start_report(p->model, step, out, max);
        if (n > 0) return n;
        /* then the pad's lights, once */
        if (step == 0 && (p->model == USBPAD_DS4 || p->model == USBPAD_DS3)) return build_output(p, out, max);
        return 0;
    }
    default:
        return 0;
    }
}

/* The profile's own wake-up sequence (the Switch's subcommands), as the
 * Bluetooth host runs it: step by step, repeated until the full report. */
static void profile_init_tick(wired_pad *p, long now)
{
    unsigned char buf[128];
    int n;

    if (p->kind != WIRED_HID || !p->prof || !p->prof->init_report || p->prof_done) return;
    if (p->model == USBPAD_DS4) {       /* its wake-up is the Bluetooth output report */
        p->prof_done = 1;
        return;
    }
    if (p->prof_step < 0) {             /* between rounds */
        if (p->full) p->prof_done = 1;
        else if (now - p->t_prof >= T_PROF_RETRY) p->prof_step = 0;
        return;
    }
    if (now - p->t_prof < T_PROF_STEP) return;
    n = p->prof->init_report(p->pctx.bytes, p->prof_step, &p->out, buf, (int)sizeof buf);
    if (n > 0) {
        if (pad_send(p, buf, n)) {
            p->prof_step++;
            p->t_prof = now;
        }
        return;
    }
    p->prof_rounds++;
    if (!p->prof->needs_full || p->full || p->prof_rounds >= PROF_ROUNDS) {
        if (p->prof->needs_full && !p->full) log_line("usb pad %s: never switched to its full report", p->node);
        p->prof_done = 1;
    } else {
        p->prof_step = -1;
        p->t_prof = now;
    }
}

/* ---- lifecycle ----------------------------------------------------------- */

static void pad_close(wired_pad *p, const char *why)
{
    struct usb_fs_uninit un;

    if (p->fd >= 0) {
        memset(&un, 0, sizeof un);
        ioctl(p->fd, USB_FS_UNINIT, &un);
        close(p->fd);
    }
    log_line("usb pad %s: %s", p->node, why);
    if (p->announced && g_ev.on_disconnect) g_ev.on_disconnect(g_ev.ud, p->slot);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

static void begin_init(wired_pad *p, long now)
{
    p->init_step = 0;
    p->prof_step = 0;
    p->prof_rounds = 0;
    p->prof_done = 0;
    p->t_prof = 0;
    p->t_keepalive = now;
}

/* Configures, claims and starts initialising an open node. */
static int bring_up(wired_pad *p, long now)
{
    int cfg = 0;
    usb_ctl ctl;
    unsigned char d[64];
    int n;

    ioctl(p->fd, USB_SET_CONFIG, &cfg);
    ioctl(p->fd, USB_CLAIM_INTERFACE, &p->lay.iface);
    if (!fs_setup(p)) return 0;

    if (p->kind == WIRED_HID && (n = usb_quirks_wake_control(p->model, &ctl, d, (int)sizeof d)) > 0) {
        if (control(p->fd, ctl.request_type, ctl.request, ctl.value,
                    ctl.index ? ctl.index : (uint16_t)p->lay.iface, d, n) < 0)
            log_line("usb pad %s: the wake-up request failed: %s", p->node, strerror(errno));
    }
    if (p->kind == WIRED_XINPUT) {
        /* Some 2.4 GHz dongles stay dormant until they are asked anything. */
        control(p->fd, 0xC1, 0x01, 0x0100, 0x0000, d, 20);
    }
    p->state = WIRED_ST_INIT;
    p->t_report = now;
    begin_init(p, now);
    return 1;
}

static int rebuild(wired_pad *p, long now)
{
    struct usb_fs_uninit un;

    memset(&un, 0, sizeof un);
    ioctl(p->fd, USB_FS_UNINIT, &un);
    close(p->fd);
    p->fd = open(p->node, O_RDWR);
    if (p->fd < 0) return 0;
    return bring_up(p, now);
}

static wired_pad *pad_by_node(const char *node)
{
    int i;

    for (i = 0; i < WIRED_MAX_PADS; i++)
        if (g_pad[i].used && strcmp(g_pad[i].node, node) == 0) return &g_pad[i];
    return NULL;
}

static int free_pad(void)
{
    int i;

    for (i = 0; i < WIRED_MAX_PADS; i++) if (!g_pad[i].used) return i;
    return -1;
}

static int ignored(const char *node, long now)
{
    size_t i;

    for (i = 0; i < sizeof g_ignore / sizeof g_ignore[0]; i++)
        if (strcmp(g_ignore[i].node, node) == 0) return now < g_ignore[i].until;
    return 0;
}

static void ignore(const char *node, long now)
{
    size_t i, oldest = 0;

    for (i = 0; i < sizeof g_ignore / sizeof g_ignore[0]; i++) {
        if (strcmp(g_ignore[i].node, node) == 0 || !g_ignore[i].node[0]) { oldest = i; break; }
        if (g_ignore[i].until < g_ignore[oldest].until) oldest = i;
    }
    snprintf(g_ignore[oldest].node, sizeof g_ignore[oldest].node, "%s", node);
    g_ignore[oldest].until = now + T_IGNORE;
}

static const char *pad_name(const wired_pad *p)
{
    const char *n = "";

    switch (p->kind) {
    case WIRED_GIP:    n = gip_pad_name(p->vid, p->pid); break;
    case WIRED_XINPUT:
    case WIRED_X360W:  n = xinput_pad_name(p->vid, p->pid); break;
    case WIRED_HID:    n = usb_quirks_name(p->vid, p->pid); break;
    default: break;
    }
    return n;
}

static const char *pad_profile_name(const wired_pad *p)
{
    const char *n = pad_name(p);

    if (n[0]) return n;
    switch (p->kind) {
    case WIRED_GIP:    return "Xbox (USB)";
    case WIRED_XINPUT: return "XInput pad (USB)";
    case WIRED_X360W:  return "Xbox 360 (wireless)";
    default:           return p->prof ? p->prof->name : "HID pad (USB)";
    }
}

/* Reads a HID interface's report descriptor into desc. Returns its length, 0. */
static int read_report_descriptor(int fd, const layout *lay, unsigned char *desc, int max)
{
    int want = lay->hid_desc_len > 0 && lay->hid_desc_len <= max ? lay->hid_desc_len : max;
    int n = control(fd, 0x81, 0x06, 0x2200, (uint16_t)lay->iface, desc, want);

    return n > 0 ? n : 0;
}

/* Looks at one device node; takes it if it is a game pad not yet ours. */
static void try_node(const char *node, long now)
{
    struct usb_gen_descriptor gd;
    struct usb_device_descriptor dd;
    unsigned char cfg[DESC_MAX], desc[DESC_MAX];
    hci_function hf[HCI_FUNCTIONS_MAX];
    layout lay;
    wired_pad *p;
    int fd, i, kind, cfglen, model = USBPAD_HID;
    const pad_profile *prof = NULL;
    unsigned vid = 0, pid = 0;
    union { unsigned char bytes[PROFILE_CTX_MAX]; uint64_t align; } pctx;

    if (g_skip[0] && strcmp(node, g_skip) == 0) return;
    if (pad_by_node(node) || ignored(node, now)) return;
    if (access(node, F_OK) != 0) return;
    fd = open(node, O_RDWR);
    if (fd < 0) return;
    memset(&gd, 0, sizeof gd);
    gd.ugd_data = cfg;
    gd.ugd_maxlen = (uint16_t)sizeof cfg;
    gd.ugd_config_index = 0;
    if (ioctl(fd, USB_GET_FULL_DESC, &gd) != 0 || gd.ugd_actlen == 0) {
        close(fd);
        ignore(node, now);
        return;
    }
    cfglen = (int)gd.ugd_actlen < (int)sizeof cfg ? (int)gd.ugd_actlen : (int)sizeof cfg;
    if (usb_find_hci_functions(cfg, cfglen, hf) > 0) {      /* never a Bluetooth chip */
        close(fd);
        ignore(node, now);
        return;
    }
    memset(&dd, 0, sizeof dd);
    if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0) {
        vid = UGETW(dd.idVendor);
        pid = UGETW(dd.idProduct);
    }

    /* Which kind: by the interface the device presents. */
    if (find_iface(cfg, cfglen, 0xFF, 0x47, 0xD0, &lay)) {
        kind = WIRED_GIP;
    } else if (find_iface(cfg, cfglen, 0xFF, 0x5D, 0x01, &lay)) {
        kind = WIRED_XINPUT;
    } else if (find_iface(cfg, cfglen, 0xFF, 0x5D, 0x81, &lay)) {
        kind = WIRED_X360W;
    } else if (find_iface(cfg, cfglen, 0x03, 0x00, -1, &lay) || find_iface(cfg, cfglen, 0x03, -1, 0x00, &lay)) {
        kind = WIRED_HID;
        model = usb_quirks_model(vid, pid);
        if (model != USBPAD_HID) prof = profile_find((uint16_t)vid, (uint16_t)pid);
        if (!prof) {
            /* Anything else: a game pad if its report descriptor says so. */
            int dlen = read_report_descriptor(fd, &lay, desc, (int)sizeof desc);
            if (dlen <= 0 || !generic_setup(pctx.bytes, desc, dlen, (uint16_t)vid, (uint16_t)pid, g_map_dir)) {
                close(fd);
                ignore(node, now);
                return;
            }
            prof = &generic_profile;
            model = USBPAD_HID;
        }
    } else {
        close(fd);
        ignore(node, now);
        return;
    }

    i = free_pad();
    if (i < 0) {
        close(fd);
        return;
    }
    p = &g_pad[i];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->fd = fd;
    snprintf(p->node, sizeof p->node, "%s", node);
    p->lay = lay;
    p->kind = kind;
    p->model = model;
    p->prof = prof;
    if (prof == &generic_profile) memcpy(&p->pctx, &pctx, sizeof pctx);
    else if (prof && prof->setup) prof->setup(p->pctx.bytes, (uint16_t)vid, (uint16_t)pid);
    p->slot = g_first_slot + i;
    p->vid = vid;
    p->pid = pid;
    p->out.player = (uint8_t)(i + 1);
    switch (i) {                        /* the light bar colour, as the Bluetooth slots */
    case 0: p->out.b = 0x40; break;
    case 1: p->out.r = 0x40; break;
    case 2: p->out.g = 0x40; break;
    default: p->out.r = 0x20; p->out.b = 0x20; break;
    }
    pad_state_reset(&p->st);
    log_line("usb pad %s: %04x:%04x %s, %s (interface %d, IN %02x, OUT %02x) -> slot %d",
             node, p->vid, p->pid, pad_profile_name(p), kind_word(kind), lay.iface, lay.ep_in,
             lay.ep_out, p->slot);
    if (!bring_up(p, now)) {
        log_line("usb pad %s: transfer setup failed: %s", node, strerror(errno));
        close(p->fd);
        memset(p, 0, sizeof *p);
        p->fd = -1;
        ignore(node, now);
    }
}

static void scan(long now)
{
    char node[32];
    int bus, unit;

    for (bus = 0; bus < 8; bus++)
        for (unit = 1; unit < 16; unit++) {
            if (free_pad() < 0) return;
            snprintf(node, sizeof node, "/dev/ugen%d.%d", bus, unit);
            try_node(node, now);
        }
}

/* ---- the loop -------------------------------------------------------------- */

static void note_report(wired_pad *p, long now)
{
    p->t_report = now;
    p->reinits = 0;
    p->t_err_first = 0;
    p->rebuilt = 0;
    if (now - p->t_window >= 1000) {
        if (p->in_window >= STREAM_RATE && !p->streams) {
            p->streams = 1;
            log_line("usb pad %s: streams reports (%ld/s)", p->node, p->in_window);
        }
        p->t_window = now;
        p->in_window = 0;
    }
    p->in_window++;
}

static void deliver(wired_pad *p)
{
    p->reports++;
    if (p->state != WIRED_ST_LIVE) {
        p->state = WIRED_ST_LIVE;
        log_line("usb pad %s: input flowing", p->node);
    }
    if (!p->announced) {
        pad_info info;
        memset(&info, 0, sizeof info);
        info.vid = (uint16_t)p->vid;
        info.pid = (uint16_t)p->pid;
        info.profile = pad_profile_name(p);
        snprintf(info.name, sizeof info.name, "%s", pad_name(p));
        p->announced = 1;
        if (g_ev.on_connect) g_ev.on_connect(g_ev.ud, p->slot, &info);
    }
    if (g_ev.on_state) g_ev.on_state(g_ev.ud, p->slot, &p->st);
}

static void on_message(wired_pad *p, const unsigned char *msg, int n, long now)
{
    unsigned char ack[GIP_MSG_MAX];
    int g, r, present;

    switch (p->kind) {
    case WIRED_GIP:
        if (n < 4) return;
        note_report(p, now);
        if (msg[1] & GIP_OPT_ACK) pad_send(p, ack, gip_build_ack(msg, ack));
        if ((g = gip_parse_guide(msg, n)) >= 0) {
            p->guide = g;
            p->st.buttons = (p->st.buttons & ~(uint32_t)PAD_PS) | (g ? PAD_PS : 0);
        } else if ((g = gip_parse_battery(msg, n, &p->batt_pct, &p->batt_charging)) >= 0) {
            if (g != p->has_batt || g)
                log_line("usb pad %s: battery %s", p->node, g ? "level read" : "none (wired power)");
            p->has_batt = g;
            p->st.has_battery = (uint8_t)g;
            p->st.battery_pct = (uint8_t)p->batt_pct;
            p->st.charging = (uint8_t)p->batt_charging;
        } else if (!gip_parse_input(msg, n, p->guide, &p->st)) {
            return;
        } else if (p->has_batt) {       /* the parse reset the state */
            p->st.has_battery = 1;
            p->st.battery_pct = (uint8_t)p->batt_pct;
            p->st.charging = (uint8_t)p->batt_charging;
        }
        deliver(p);
        break;

    case WIRED_XINPUT:
        if (!xinput_parse_wired(msg, n, &p->st)) return;
        note_report(p, now);
        deliver(p);
        break;

    case WIRED_X360W:
        r = xinput_parse_receiver(msg, n, &p->st, &present);
        if (r == XINPUT_FRAME_PRESENCE) {
            note_report(p, now);
            if (present != p->present) {
                log_line("usb pad %s: a pad %s the receiver", p->node, present ? "joined" : "left");
                p->present = present;
                if (!present && p->announced) {
                    p->announced = 0;
                    p->state = WIRED_ST_INIT;
                    if (g_ev.on_disconnect) g_ev.on_disconnect(g_ev.ud, p->slot);
                } else if (present) {
                    begin_init(p, now);     /* lights for the newcomer */
                }
            }
            return;
        }
        if (r != XINPUT_FRAME_INPUT) return;
        note_report(p, now);
        p->present = 1;
        deliver(p);
        break;

    case WIRED_HID:
        if (!p->prof) return;
        r = p->prof->parse(p->pctx.bytes, msg, n, &p->st);
        if (r == PARSE_NONE) return;
        if (r == PARSE_FULL) p->full = 1;
        note_report(p, now);
        deliver(p);
        break;

    default:
        break;
    }
}

static void pad_poll(wired_pad *p, long now)
{
    const unsigned char *msg;
    unsigned char out[128];
    int n;

    /* The start-up messages, one per completed write, then the profile's. */
    if (p->init_step >= 0 && !p->pending[EP_OUT]) {
        n = build_start(p, p->init_step, out, (int)sizeof out);
        if (n <= 0) p->init_step = -1;
        else if (pad_send(p, out, n)) p->init_step++;
    }
    if (p->init_step < 0) profile_init_tick(p, now);

    fs_start(p, EP_IN, p->lay.mps_in ? p->lay.mps_in : 64);
    n = fs_poll(p);
    if (n > 0) {
        msg = p->data[EP_IN];
        on_message(p, msg, n, now);
    } else if (n < 0) {
        if (!p->t_err_first) p->t_err_first = now;
    } else {
        p->t_err_first = 0;             /* a quiet endpoint is a healthy one */
    }
    if (p->pending[EP_OUT] && now - p->t_out > 1000) p->pending[EP_OUT] = 0;

    /* Gone: detached, its node vanished, or real transfer errors go on. */
    if (p->gone || access(p->node, F_OK) != 0) {
        pad_close(p, "unplugged");
        return;
    }
    if (p->t_err_first && now - p->t_err_first >= T_ERRORS) {
        if (!p->rebuilt) {
            p->rebuilt = 1;
            p->t_err_first = 0;
            log_line("usb pad %s: transfers failing for %d s, rebuilding the USB connection", p->node, T_ERRORS / 1000);
            if (!rebuild(p, now)) pad_close(p, "could not be reopened, gone");
        } else {
            pad_close(p, "transfers keep failing, gone");
        }
        return;
    }

    if (p->kind == WIRED_GIP && g_keepalive_ms && now - p->t_keepalive >= g_keepalive_ms) begin_init(p, now);

    /* A streaming pad that went quiet is stalled; an on-change pad is idle. */
    if (p->streams && now - p->t_report >= T_SILENT && now - p->t_reinit >= T_SILENT) {
        p->t_reinit = now;
        if (p->state == WIRED_ST_LIVE) {
            p->state = WIRED_ST_SILENT;
            log_line("usb pad %s: streamed, now no input for %d s, re-initialising", p->node, T_SILENT / 1000);
        }
        begin_init(p, now);
        if (++p->reinits >= REINITS_BEFORE_REBUILD) {
            p->reinits = 0;
            log_line("usb pad %s: re-initialising is not helping, rebuilding the USB connection", p->node);
            if (!rebuild(p, now)) pad_close(p, "could not be reopened, gone");
        }
    }
}

void usb_pads_start(const host_events *ev, int first_slot, const char *skip_node,
                    const char *map_dir, int keepalive_s)
{
    int i;

    g_ev = *ev;
    g_first_slot = first_slot;
    g_keepalive_ms = keepalive_s > 0 ? keepalive_s * 1000 : 0;
    snprintf(g_skip, sizeof g_skip, "%s", skip_node ? skip_node : "");
    snprintf(g_map_dir, sizeof g_map_dir, "%s", map_dir ? map_dir : ".");
    for (i = 0; i < WIRED_MAX_PADS; i++) {
        memset(&g_pad[i], 0, sizeof g_pad[i]);
        g_pad[i].fd = -1;
    }
    memset(g_ignore, 0, sizeof g_ignore);
    g_t_scan = 0;
    log_line("usb pads: watching for wired controllers (Xbox, XInput, DualShock, Switch, HID)%s",
             g_keepalive_ms ? " (with GIP keepalive)" : "");
}

void usb_pads_poll(long now)
{
    int i;

    if (g_first_slot < 0) return;
    if (now - g_t_scan >= T_RESCAN) {
        g_t_scan = now;
        scan(now);
    }
    for (i = 0; i < WIRED_MAX_PADS; i++)
        if (g_pad[i].used) pad_poll(&g_pad[i], now);
}

void usb_pads_stop(void)
{
    int i;

    if (g_first_slot < 0) return;
    for (i = 0; i < WIRED_MAX_PADS; i++)
        if (g_pad[i].used) pad_close(&g_pad[i], "closed");
    g_first_slot = -1;
}

int usb_pads_set_output(int slot, const pad_output *out)
{
    unsigned char m[128];
    int k, n;

    for (k = 0; k < WIRED_MAX_PADS; k++) {
        wired_pad *p = &g_pad[k];
        if (!p->used || p->slot != slot || p->state != WIRED_ST_LIVE) continue;
        p->out.strong = out->strong;
        p->out.weak = out->weak;
        n = build_output(p, m, (int)sizeof m);
        return n > 0 && pad_send(p, m, n);
    }
    return 0;
}

int usb_pads_get(int i, wired_pad_info *info, pad_state *st)
{
    int k;

    for (k = 0; k < WIRED_MAX_PADS; k++) {
        const wired_pad *p = &g_pad[k];
        if (!p->used) continue;
        if (i-- > 0) continue;
        memset(info, 0, sizeof *info);
        snprintf(info->node, sizeof info->node, "%s", p->node);
        info->vid = p->vid;
        info->pid = p->pid;
        info->name = pad_profile_name(p);
        info->kind = kind_word(p->kind);
        info->slot = p->slot;
        info->state = p->state;
        info->reports = p->reports;
        if (st) *st = p->st;
        return 1;
    }
    return 0;
}
