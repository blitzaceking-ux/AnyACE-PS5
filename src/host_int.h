/* The host's internals, shared by the Classic (host.c) and LE (le.c) halves
 * of the stack. Not for use outside them. */
#ifndef PH_HOST_INT_H
#define PH_HOST_INT_H

#include "host.h"
#include "profiles.h"

#include <stdint.h>

/* ---- protocol constants ------------------------------------------------- */

/* HCI opcodes: OGF << 10 | OCF. */
#define OP_INQUIRY              0x0401
#define OP_INQUIRY_CANCEL       0x0402
#define OP_CREATE_CONNECTION    0x0405
#define OP_DISCONNECT           0x0406
#define OP_ACCEPT_CONNECTION    0x0409
#define OP_REJECT_CONNECTION    0x040A
#define OP_LINK_KEY_REPLY       0x040B
#define OP_LINK_KEY_NEG_REPLY   0x040C
#define OP_PIN_CODE_REPLY       0x040D
#define OP_AUTH_REQUESTED       0x0411
#define OP_SET_ENCRYPTION       0x0413
#define OP_IO_CAP_REPLY         0x042B
#define OP_USER_CONFIRM_REPLY   0x042C
#define OP_SET_EVENT_MASK       0x0C01
#define OP_WRITE_LOCAL_NAME     0x0C13
#define OP_WRITE_SCAN_ENABLE    0x0C1A
#define OP_WRITE_CLASS_OF_DEV   0x0C24
#define OP_WRITE_INQUIRY_MODE   0x0C45
#define OP_WRITE_SSP_MODE       0x0C56
#define OP_REMOTE_NAME_REQ      0x0419
#define OP_LE_CREATE_CONN_CANCEL_OP 0x200E
#define OP_READ_BUFFER_SIZE     0x1005
#define OP_READ_BD_ADDR         0x1009
#define OP_READ_SCAN_ENABLE     0x0C19
#define OP_READ_SSP_MODE        0x0C55

#define CID_SIGNALING   0x0001
#define PSM_SDP         0x0001
#define PSM_HID_CTRL    0x0011
#define PSM_HID_INTR    0x0013

/* Our ends of a link's channels (CIDs are per link). */
#define CID_SDP         0x0040
#define CID_CTRL        0x0041
#define CID_INTR        0x0042

/* L2CAP signaling codes. */
#define SIG_REJECT      0x01
#define SIG_CONN_REQ    0x02
#define SIG_CONN_RSP    0x03
#define SIG_CFG_REQ     0x04
#define SIG_CFG_RSP     0x05
#define SIG_DISC_REQ    0x06
#define SIG_DISC_RSP    0x07
#define SIG_ECHO_REQ    0x08
#define SIG_ECHO_RSP    0x09
#define SIG_INFO_REQ    0x0A
#define SIG_INFO_RSP    0x0B

/* HID transaction headers. */
#define HID_DATA_INPUT  0xA1
#define HID_DATA_OUTPUT 0xA2
#define HID_CONTROL_UNPLUG 0x15

/* ---- timing (ms) -------------------------------------------------------- */

#define T_SELF_AUTH      800    /* a reconnecting pad may authenticate itself */
#define T_SELF_ENCRYPT  1500    /* ... and start encryption itself */
#define T_SELF_CHANNELS 1200    /* ... and open its own HID channels */
#define T_INTR_AFTER     500    /* interrupt channel after control */
#define T_CONN_TIMEOUT 20000    /* paging a pad */
#define T_SETUP_TIMEOUT 30000   /* connection to ready */
#define T_CHAN_CONNECT  2500
#define T_CFG_RESEND    1200
#define CFG_TRIES          6
#define T_CFG_TIMEOUT   4000    /* the pad's own request may have been lost */
#define T_SDP_RESEND    2000
#define SDP_TRIES          3
#define T_INIT_STEP       60    /* between wake-up reports */
#define T_INIT_RETRY     800    /* before repeating an unanswered sequence */
#define INIT_ROUNDS        5

#define SDP_Q_IDS          1    /* Device ID: vendor and product */
#define SDP_Q_DESC         2    /* HID report descriptor, for the generic profile */
#define T_PROBE_IDLE   20000    /* nothing heard for this long: probe */
#define T_PROBE_WAIT    2000    /* between probes */
#define PROBE_TRIES        3

/* ACL flow control on the shared controller: a few Number Of Completed
 * Packets reports go to the system's driver, so a packet without a report
 * after INFLIGHT_MS is taken as sent. */
#define INFLIGHT_MAX      64
#define INFLIGHT_MS       60

/* Devices a pairing search found, shown on the web page until the user
 * picks one. Nothing is paired without that pick. */
#define FOUND_MAX          8
#define FOUND_TTL      60000    /* not seen for this long: gone */
#define T_NAME_WAIT     5000    /* a name request the controller never answers */
#define SYS_MAX            8    /* addresses the console's own driver uses */
#define T_PICK_GRACE   60000    /* a link we asked for may complete this late */
#define T_PICK_RETRY    4000    /* between tries at a picked pad */
#define PICK_TRIES         4    /* connection tries in a row before the pick is given up */
#define PICK_TRIES_TOTAL  24    /* tries of any kind, whatever happens between them */
#define T_PICK_FRESH    1500    /* page a picked pad only this soon after it answered a search */
#define T_PICK_EXTEND  25000    /* a pick keeps the search going at least this long */

#define DB_MAX            16
#define DB_REC_02         64    /* one record in a format-02 store (no name) */
#define DB_REC           128    /* one record in the store file: 02's 64 bytes, then the name */
#define DB_MAGIC     "ANYPAD03"
#define DB_MAGIC_02  "ANYPAD02"

/* ---- state -------------------------------------------------------------- */

enum { NAME_NONE, NAME_ASKED, NAME_KNOWN, NAME_FAILED };

/* A device seen by a pairing search: Classic (from an inquiry record, kept
 * to page it with) or LE (from an advert). */
typedef struct {
    int used;
    unsigned char addr[6];
    unsigned char rec[14];          /* Classic: inquiry record, RSSI layout */
    int le;
    uint8_t addr_type;              /* LE */
    uint32_t cod;                   /* Classic */
    int rssi;
    char name[HOST_FOUND_NAME + 1];
    int name_state;                 /* NAME_* */
    long t_seen, t_name;
} found_t;

typedef enum { CH_CLOSED, CH_CONNECTING, CH_CONFIG, CH_OPEN } chan_state;

typedef struct {
    unsigned psm, scid, dcid;
    chan_state st;
    int cfg_ours_ok;        /* the pad accepted our configuration */
    int cfg_theirs_ok;      /* we accepted the pad's */
    int cfg_tries;
    long t_state, t_cfg;
} chan;

/* A paired pad. Classic pads keep a link key; LE pads a long-term key
 * (with its EDIV and Rand under legacy pairing) and possibly an identity
 * resolving key, to recognise them behind private addresses. */
typedef struct {
    unsigned char addr[6], key[16], type;
    uint16_t vid, pid;
    uint8_t le, addr_type;
    uint16_t ediv;
    uint8_t rand[8], irk[16], has_irk;
    char name[HOST_FOUND_NAME + 1];     /* its Bluetooth name when paired, "" unknown */
} db_rec;

typedef struct {
    int credits, max;
    long inflight[INFLIGHT_MAX];
    int head, count;
} acl_pool;

#define LE_MAX_CHARS 24

typedef struct {
    uint16_t decl, value, end, uuid;
    uint8_t props;
    uint8_t rid, rtype;             /* from its Report Reference */
    uint16_t ref, cccd;             /* descriptor handles, 0 if none */
} gatt_char;

/* An LE pad's pairing (SMP) and GATT discovery. Crypto values are kept
 * most significant byte first, as the specification writes them. */
typedef struct {
    uint8_t addr_type;

    int smp;                        /* SMP_* */
    int sc;                         /* LE Secure Connections */
    uint8_t preq[7], pres[7];       /* the pairing commands, as sent */
    uint8_t na[16], nb[16], cb[16];
    uint8_t pkb[64];                /* the pad's public key, as sent */
    uint8_t dhkey[32];              /* least significant byte first */
    int have_pkb, have_cb, have_nb, have_dhkey, dhkey_asked;
    uint8_t ltk[16];                /* the key the link is encrypted with */
    uint8_t keys_expected, keys_got;
    uint8_t dist_ltk[16], dist_rand[8], dist_irk[16], id_addr[6], id_type;
    uint16_t dist_ediv;
    int enc_sent, bonded;
    long t_smp, t_enc;

    int gatt;                       /* G_* */
    uint16_t mtu;
    uint8_t req[32];
    int req_len, req_tries;
    long t_req;
    uint16_t svc_start, svc_end, cursor;
    gatt_char ch[LE_MAX_CHARS];
    int nch, idx;
    uint16_t map_handle;
    uint8_t map[1024];
    int map_len;

    /* The Battery Service, read once discovery is done and then now and
     * then: 0 not looked for, 1 asked, 2 known, 3 reading, 9 none. */
    int batt_state;
    uint16_t batt_handle;
    long t_batt;
    int batt_pct;                   /* -1 unknown */
} le_link;

typedef struct {
    int used;
    unsigned char addr[6];
    int connected;          /* the ACL link is up */
    unsigned handle;
    int pairing;            /* we found it in pairing mode */
    long t_create, t_conn;

    int auth_sent, auth_ok, enc_sent, enc_on;
    long t_auth_ok, t_secure;

    int ids_known;
    uint16_t vid, pid;
    const pad_profile *prof;

    int sdp_query;                  /* SDP_Q_* being asked, 0: none */
    int sdp_sent, sdp_tries;
    unsigned sdp_tid;
    long t_sdp;
    unsigned char sdp_cont[16];     /* continuation of a long answer */
    int sdp_cont_len;
    unsigned char sdp_buf[1024];    /* attribute lists gathered so far */
    int sdp_len;

    chan sdp, ctrl, intr;
    int ready, disconnecting;

    unsigned char rx[1024];
    int rx_len, rx_need;

    pad_state st;
    pad_output out;
    long t_out;
    long n_reports;                 /* input reports parsed, for the page */
    uint8_t last_rid;               /* id of the last one */
    long n_other;                   /* reports the profile did not take */
    uint8_t rid_other;              /* id of the last of those */
    int n_logged;                   /* of those, how many were logged in full */
    char name[HOST_FOUND_NAME + 1]; /* from the search that found it, "" unknown */

    /* The profile's own memory, and its wake-up sequence. */
    union { unsigned char bytes[PROFILE_CTX_MAX]; uint64_t align; } pctx;
    int init_step, init_rounds, init_done, full;
    long t_init;

    int le;                         /* a Bluetooth LE link */
    le_link le_s;
} link_t;

struct host {
    hci_t hci;
    host_events ev;
    char db_path[256];
    char map_dir[256];
    db_rec db[DB_MAX];
    int ndb;

    link_t links[HOST_MAX_PADS];
    unsigned char sig_id;

    unsigned cc_op;
    unsigned char cc[HCI_PKT_MAX];
    int cc_len;

    unsigned acl_mtu;
    acl_pool br;                    /* the controller's ACL buffers */

    /* Bluetooth LE (le.c) */
    int le_ok;                      /* the controller does LE */
    int le_separate;                /* LE has its own buffers */
    unsigned le_mtu;
    acl_pool lep;
    unsigned char bdaddr[6];        /* our address */
    unsigned char le_pk[64];        /* our P-256 public key, as sent */
    int le_pk_ok;
    int le_scanning, le_scan_want;
    long t_scan;
    link_t *dhkey_owner;            /* the link a DHKey is being made for */

    long pair_until;
    found_t found[FOUND_MAX];
    int name_asking;                /* a name request is out (Classic) */
    unsigned char sys_addr[SYS_MAX][6];     /* links the console's driver made */
    int sys_handle[SYS_MAX];                /* and their handles, -1 once gone */
    int nsys;
    unsigned char pick_addr[6];     /* the last device the user picked */
    long pick_at;
    int pick_wait, pick_force;      /* that pick waits for its name */
    int pick_want;                  /* and is wanted until the window closes */
    unsigned char pick_rec[14];     /* its inquiry record, to page it again */
    char pick_name[HOST_FOUND_NAME + 1];
    int pick_state, pick_why, pick_tries;   /* HOST_PICK_*, HOST_WHY_* */
    int pick_total;
    long pick_try_at;
    long purge_at;                  /* when stale links were last cleared */
    int inquiring;
    int dead;               /* the transport is gone */
    int diag_done;          /* the transport's self-report was logged */
    int scan_orig, scan_set; /* page scan as found, and as we set it (-1: untouched) */
    long last_rx, probe_at; /* liveness: anything heard, last probe */
    int probes;
};


/* ---- shared between host.c and le.c ---- */

const char *addr_str(const unsigned char *a);
int  send_cmd(host_t *h, unsigned op, const void *p, int len);
int  hci_sync(host_t *h, unsigned op, const void *p, int plen);
int  slot_of(host_t *h, const link_t *l);
link_t *link_by_addr(host_t *h, const unsigned char *a);
link_t *link_new(host_t *h, const unsigned char *addr);
void link_free(host_t *h, link_t *l);
void link_disconnect(host_t *h, link_t *l, const char *why);
void link_ready(host_t *h, link_t *l);
void link_input(host_t *h, link_t *l, const unsigned char *rep, int len);
int  l2_send(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len);
void init_tick(host_t *h, link_t *l, long now);

/* Records a device a pairing search saw (host.c); le.c reports adverts. */
found_t *found_add(host_t *h, const unsigned char *addr, int le, uint8_t addr_type, int rssi);
found_t *found_find_le(host_t *h, const unsigned char *addr);
int pick_wanted(const host_t *h, const unsigned char *addr);
/* The pick's bookkeeping, shared with le.c: a try is about to start (paces
 * and counts them; 0 if not now, or given up), a try failed, the chip's
 * stale links are closed. */
int  pick_try_begin(host_t *h, const unsigned char *addr);
void pick_connected(host_t *h, const unsigned char *addr);
void pick_failed_try(host_t *h, const unsigned char *addr, int why);
void purge_stale_links(host_t *h);
void found_name_from_ad(found_t *f, const unsigned char *d, int n);

db_rec *db_find(host_t *h, const unsigned char *addr);
db_rec *db_put(host_t *h, const unsigned char *addr);
/* db_put, and the link's name into a record that has none. */
db_rec *db_put_link(host_t *h, const unsigned char *addr, const link_t *l);
void link_set_name(host_t *h, const unsigned char *addr, const char *name);
void db_save(host_t *h);
void db_forget(host_t *h, const unsigned char *addr);

/* le.c */
void le_setup(host_t *h);
void le_on_meta(host_t *h, const unsigned char *ev, int len);
int  le_pair(host_t *h, uint8_t type, const unsigned char *addr);
void le_on_frame(host_t *h, link_t *l, unsigned cid, const unsigned char *d, int len);
void le_on_encryption(host_t *h, link_t *l, int status, int on);
void le_step(host_t *h, link_t *l, long now);
void le_poll(host_t *h, long now);
void le_shutdown(host_t *h);
int  le_send_report(host_t *h, link_t *l, const unsigned char *rep, int len);

#endif
