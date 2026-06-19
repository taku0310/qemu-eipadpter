/*
 * eip_adapter.c - EtherNet/IP adapter (CIP target) with Class 1 implicit I/O
 *
 * Acts as an EtherNet/IP adapter device. An originator (PLC / scanner) opens
 * a Class 1 (cyclic, implicit) connection via Forward Open over TCP/44818;
 * real-time I/O then flows over UDP/2222:
 *
 *     O->T  (originator -> target) : the adapter CONSUMES output data
 *     T->O  (target -> originator) : the adapter PRODUCES input data, cyclically
 *
 * Runs as a normal Linux userspace program on an Ubuntu/Linux host. The well
 * known ports it uses (TCP/UDP 44818, UDP 2222) are unprivileged.
 *
 * This is a single-threaded poll() based implementation handling:
 *   - TCP/44818 encapsulation server  (RegisterSession, SendRRData, ...)
 *   - UDP/44818 ListIdentity discovery (broadcast)
 *   - UDP/2222   Class 1 implicit I/O  (cyclic produce + consume)
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "eip.h"
#include "device.h"

/* -------------------------------------------------------------------------- */
/* configuration & globals                                                    */
/* -------------------------------------------------------------------------- */

#define MAX_TCP_CLIENTS 16
#define MAX_CONNS        8
#define BUF_SIZE       2048

static volatile sig_atomic_t g_run = 1;
static int  g_verbose = 1;
static int  g_ot_run_idle = 1;   /* expect 32-bit run/idle header on O->T  */
static int  g_to_run_idle = 0;   /* prepend 32-bit run/idle header on T->O */
static int  g_loopback   = 1;    /* mirror consumed O->T bytes into T->O   */
static uint16_t g_out_inst = ASM_OUTPUT_INST;
static uint16_t g_in_inst  = ASM_INPUT_INST;
static uint16_t g_cfg_inst = ASM_CONFIG_INST;

/* ---- network binding ---- */
static uint32_t g_bind_be  = INADDR_ANY;  /* socket bind address (network order) */

/* ---- I/O size policy: 0 = accept whatever the originator requests ---- */
static uint16_t g_exp_out_size = 0;       /* expected O->T (consumed) bytes      */
static uint16_t g_exp_in_size  = 0;       /* expected T->O (produced) bytes      */

/* ---- RPI acceptance bounds (microseconds): 0 = no limit ---- */
static uint32_t g_rpi_min_us = 0;
static uint32_t g_rpi_max_us = 0;

/* ---- inactivity timeout override (microseconds): 0 = derive from FO ---- */
static uint32_t g_timeout_us = 0;

/* ---- Identity object (runtime-configurable; defaults from device.h) ---- */
static uint16_t g_vendor_id    = DEV_VENDOR_ID;
static uint16_t g_device_type  = DEV_DEVICE_TYPE;
static uint16_t g_product_code = DEV_PRODUCT_CODE;
static uint8_t  g_rev_major    = DEV_REV_MAJOR;
static uint8_t  g_rev_minor    = DEV_REV_MINOR;
static uint16_t g_dev_status   = DEV_STATUS;
static uint32_t g_serial       = DEV_SERIAL;
static char     g_product_name[64] = DEV_PRODUCT_NAME;
static char     g_vendor_name[64]  = "OpenSource";
static uint32_t g_rpi_default_us   = 10000;  /* RPI advertised in EDS */

/* ---- which application connection kinds the adapter accepts ---- */
static int g_allow_exclusive   = 1;
static int g_allow_input_only  = 1;
static int g_allow_listen_only = 1;

static void logmsg(const char *fmt, ...) {
    if (!g_verbose) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    char t[16];
    strftime(t, sizeof(t), "%H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] ", t, ts.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void hexdump(const char *tag, const uint8_t *d, int n) {
    if (!g_verbose) return;
    fprintf(stderr, "    %s [%d]:", tag, n);
    for (int i = 0; i < n && i < 32; i++) fprintf(stderr, " %02x", d[i]);
    if (n > 32) fprintf(stderr, " ...");
    fputc('\n', stderr);
}

/* -------------------------------------------------------------------------- */
/* Class 1 connection state                                                   */
/* -------------------------------------------------------------------------- */

/* Application connection kinds (CIP Connection Manager) */
typedef enum {
    CONN_EXCLUSIVE,    /* owns O->T outputs and produces T->O inputs        */
    CONN_INPUT_ONLY,   /* heartbeat O->T, produces T->O; does not own       */
    CONN_LISTEN_ONLY   /* heartbeat O->T, listens to shared T->O production */
} conn_kind_t;

static const char *kind_name(conn_kind_t k) {
    switch (k) {
    case CONN_EXCLUSIVE:   return "Exclusive-Owner";
    case CONN_INPUT_ONLY:  return "Input-Only";
    case CONN_LISTEN_ONLY: return "Listen-Only";
    }
    return "?";
}

typedef struct {
    int      active;
    conn_kind_t kind;
    uint32_t ot_conn_id;     /* O->T connection id (we consume on this)   */
    uint32_t to_conn_id;     /* T->O connection id (we produce, assigned) */
    uint16_t conn_serial;
    uint16_t vendor_id;
    uint32_t orig_serial;
    uint32_t ot_rpi_us;      /* requested packet interval, microseconds   */
    uint32_t to_rpi_us;
    uint16_t ot_size;        /* consumed data size (bytes)                */
    uint16_t to_size;        /* produced data size (bytes)                */
    int      to_multicast;   /* T->O served by the shared multicast stream */
    uint8_t  conn_tmo_mult;  /* connection timeout multiplier code        */
    uint32_t timeout_us;     /* inactivity timeout for O->T               */

    uint32_t to_seq32;       /* sequenced-address sequence number         */
    uint16_t to_seq16;       /* CIP transport-class-1 sequence count      */

    struct sockaddr_in peer; /* originator UDP I/O endpoint               */
    struct timespec next_send;
    struct timespec last_recv;
} conn_t;

static conn_t g_conns[MAX_CONNS];

/* Shared device I/O images: one output (written by the exclusive owner) and
 * one input (produced to every T->O consumer). */
static uint8_t  g_out_image[512];   /* O->T consumed (owner outputs)        */
static uint16_t g_out_len = 0;      /* valid bytes in g_out_image           */
static uint32_t g_heartbeat = 0;    /* demo counter placed in produced data */

/* Shared multicast T->O production. A single multicast stream is shared by the
 * exclusive owner and any listen-only connections that join it. */
static int       g_io_fd = -1;            /* UDP/2222 socket (for mcast opts) */
static int       g_mcast_active = 0;
static uint32_t  g_mcast_addr = 0;        /* group address (network order)    */
static uint32_t  g_mcast_conn_id = 0;     /* shared T->O connection id        */
static uint32_t  g_mcast_rpi_us = 0;
static uint16_t  g_mcast_size = 0;
static uint32_t  g_mcast_seq32 = 0;
static uint16_t  g_mcast_seq16 = 0;
static struct timespec g_mcast_next_send;
static uint8_t   g_mcast_ttl = 1;

/* For a Forward Open reply: network-order multicast addr to advertise as the
 * T->O socket address item, or 0 when no item is needed. */
static uint32_t  g_fo_sockaddr_to = 0;

static uint32_t  g_my_ip_be = 0;   /* our IP (network order); 0 => unknown */

static conn_t *conn_find_by_ot(uint32_t ot_id) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].active && g_conns[i].ot_conn_id == ot_id)
            return &g_conns[i];
    return NULL;
}
static conn_t *conn_find_by_serial(uint16_t serial, uint16_t vid, uint32_t osn) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].active && g_conns[i].conn_serial == serial &&
            g_conns[i].vendor_id == vid && g_conns[i].orig_serial == osn)
            return &g_conns[i];
    return NULL;
}
static conn_t *conn_alloc(void) {
    for (int i = 0; i < MAX_CONNS; i++)
        if (!g_conns[i].active) { memset(&g_conns[i], 0, sizeof(conn_t)); return &g_conns[i]; }
    return NULL;
}
static int conn_count_kind(conn_kind_t k) {
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++)
        if (g_conns[i].active && g_conns[i].kind == k) n++;
    return n;
}
/* A "producing" connection drives the shared T->O stream that listen-only
 * connections depend on (exclusive owner or input-only). */
static int conn_count_producers(void) {
    return conn_count_kind(CONN_EXCLUSIVE) + conn_count_kind(CONN_INPUT_ONLY);
}
/* Producers (owner/input-only) that drive the shared multicast production. */
static int conn_count_mcast_producers(void) {
    int n = 0;
    for (int i = 0; i < MAX_CONNS; i++) {
        conn_t *c = &g_conns[i];
        if (c->active && c->to_multicast &&
            (c->kind == CONN_EXCLUSIVE || c->kind == CONN_INPUT_ONLY)) n++;
    }
    return n;
}

/* CIP multicast address allocation (OpENer-style), assuming a /24 host part.
 * The chosen group is reported to the originator in the Forward Open reply, so
 * any valid value interoperates; this just follows the conventional formula. */
static uint32_t cip_mcast_alloc(uint32_t ip_be) {
    uint32_t ip = ntohl(ip_be);
    uint32_t host_id = ip & 0xFF;          /* /24 host part */
    if (host_id) host_id -= 1;
    host_id &= 0x3FF;
    return htonl(0xEFC00100u + (host_id << 5));   /* base 239.192.1.0 */
}

/* time helpers (monotonic) */
static void now_mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }
static long ts_diff_us(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec - b->tv_sec) * 1000000L + (a->tv_nsec - b->tv_nsec) / 1000;
}
static void ts_add_us(struct timespec *ts, uint32_t us) {
    ts->tv_nsec += (long)(us % 1000000) * 1000;
    ts->tv_sec  += us / 1000000;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_nsec -= 1000000000L; ts->tv_sec += 1; }
}

/* map connection timeout multiplier code -> multiplier value */
static uint32_t tmo_mult_value(uint8_t code) {
    static const uint32_t v[] = {4, 8, 16, 32, 64, 128, 256, 512};
    return v[code & 0x07];
}

/* -------------------------------------------------------------------------- */
/* CIP object services (explicit messaging)                                   */
/* -------------------------------------------------------------------------- */

/* append a CIP SHORT_STRING (1-byte length + chars) */
static int put_short_string(uint8_t *out, const char *s) {
    int n = (int)strlen(s);
    out[0] = (uint8_t)n;
    memcpy(out + 1, s, n);
    return n + 1;
}

/* Render the current produced (T->O / input) image into 'buf'.
 * First 4 bytes carry the demo heartbeat counter; the remainder mirrors the
 * exclusive owner's consumed outputs when loopback is enabled. */
static void build_input_image(uint8_t *buf, uint16_t size) {
    memset(buf, 0, size);
    if (size >= 4) put_u32(buf, g_heartbeat);
    if (g_loopback && size > 4 && g_out_len) {
        int n = g_out_len;
        if (n > size - 4) n = size - 4;
        memcpy(buf + 4, g_out_image, n);
    }
}

/*
 * Handle a Message Router request that is NOT a connection-management service.
 * Returns number of bytes written to 'resp' (a full MR response), or -1.
 */
static int handle_explicit(uint8_t service, uint16_t class_id, uint32_t inst,
                           uint8_t attr, const uint8_t *data, int data_len,
                           uint8_t *resp)
{
    (void)data; (void)data_len;
    int n = 4; /* MR response header filled at the end */
    uint8_t gstatus = CIP_STATUS_SUCCESS;
    uint8_t *p = resp + 4;

    if (class_id == CLASS_IDENTITY && inst == 1) {
        if (service == CIP_GET_ATTR_ALL) {
            put_u16(p, g_vendor_id);    p += 2;
            put_u16(p, g_device_type);  p += 2;
            put_u16(p, g_product_code); p += 2;
            *p++ = g_rev_major; *p++ = g_rev_minor;
            put_u16(p, g_dev_status);   p += 2;
            put_u32(p, g_serial);       p += 4;
            p += put_short_string(p, g_product_name);
            n = (int)(p - resp);
        } else if (service == CIP_GET_ATTR_SINGLE) {
            switch (attr) {
            case 1: put_u16(p, g_vendor_id);    p += 2; break;
            case 2: put_u16(p, g_device_type);  p += 2; break;
            case 3: put_u16(p, g_product_code); p += 2; break;
            case 4: *p++ = g_rev_major; *p++ = g_rev_minor; break;
            case 5: put_u16(p, g_dev_status);   p += 2; break;
            case 6: put_u32(p, g_serial);       p += 4; break;
            case 7: p += put_short_string(p, g_product_name); break;
            default: gstatus = CIP_STATUS_ATTR_UNSUPPORTED; break;
            }
            n = (int)(p - resp);
        } else {
            gstatus = CIP_STATUS_SERVICE_UNSUPPORTED;
        }
    } else if (class_id == CLASS_ASSEMBLY) {
        /* Get the current assembly image (input/output) */
        if (service == CIP_GET_ATTR_SINGLE && attr == 3) {
            if (inst == g_in_inst) {
                uint16_t sz = g_exp_in_size ? g_exp_in_size : ASM_INPUT_SIZE;
                build_input_image(p, sz); p += sz;
            } else {
                memcpy(p, g_out_image, g_out_len); p += g_out_len;
            }
            n = (int)(p - resp);
        } else {
            gstatus = CIP_STATUS_SERVICE_UNSUPPORTED;
        }
    } else {
        gstatus = CIP_STATUS_PATH_DEST_UNKNOWN;
    }

    resp[0] = service | CIP_REPLY_MASK;
    resp[1] = 0;
    resp[2] = gstatus;
    resp[3] = 0; /* additional status size (words) */
    return n;
}

/* -------------------------------------------------------------------------- */
/* EPATH parsing for the Forward Open connection path                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint16_t class_id;
    uint16_t conn_points[4];
    int      n_points;
} epath_t;

static void parse_epath(const uint8_t *p, int words, epath_t *ep) {
    memset(ep, 0, sizeof(*ep));
    int len = words * 2, i = 0;
    while (i < len) {
        uint8_t seg = p[i++];
        switch (seg) {
        case 0x20: ep->class_id = p[i++]; break;                 /* 8-bit class    */
        case 0x21: i++; ep->class_id = get_u16(p + i); i += 2; break; /* 16-bit class */
        case 0x24: if (ep->n_points < 4) ep->conn_points[ep->n_points++] = p[i]; i++; break;
        case 0x25: i++; if (ep->n_points < 4) ep->conn_points[ep->n_points++] = get_u16(p+i); i += 2; break;
        case 0x2C: if (ep->n_points < 4) ep->conn_points[ep->n_points++] = p[i]; i++; break;
        case 0x2D: i++; if (ep->n_points < 4) ep->conn_points[ep->n_points++] = get_u16(p+i); i += 2; break;
        case 0x30: i++; break;                                    /* 8-bit attribute */
        case 0x80:                                                /* data segment    */
            { uint8_t dl = p[i++]; i += dl * 2; } break;
        default:
            /* unknown segment: bail out to avoid runaway parsing */
            i = len;
            break;
        }
    }
}

/* decode connection size from network connection parameters */
static uint16_t conn_param_size(uint32_t params, int large) {
    return (uint16_t)(large ? (params & 0xFFFF) : (params & 0x01FF));
}

/* connection type from network connection parameters: 0=Null 1=Multicast 2=P2P */
#define CT_NULL      0
#define CT_MULTICAST 1
#define CT_P2P       2
static int conn_param_type(uint32_t params, int large) {
    return large ? (int)((params >> 29) & 0x3) : (int)((params >> 13) & 0x3);
}

/* -------------------------------------------------------------------------- */
/* Forward Open / Forward Close                                               */
/* -------------------------------------------------------------------------- */

static uint32_t g_next_cid = 0x20000001;

/* Connection Manager extended status codes for an unsuccessful Forward Open */
#define CM_EXT_CONN_TYPE_UNSUP   0x0103  /* transport/connection type not supported */
#define CM_EXT_OWNERSHIP         0x0106  /* ownership conflict (2nd exclusive owner) */
#define CM_EXT_RPI_NOT_SUPPORTED 0x0111
#define CM_EXT_CONN_LIMIT        0x0113
#define CM_EXT_NO_OWNER          0x0119  /* listen-only with no controlling connection */
#define CM_EXT_INVALID_OT_SIZE   0x0127
#define CM_EXT_INVALID_TO_SIZE   0x0128

/* Drop listen-only connections once no producing connection remains, and stop
 * the shared multicast production when its last producer is gone. */
static void conns_cleanup(void) {
    if (conn_count_producers() == 0) {
        for (int i = 0; i < MAX_CONNS; i++) {
            if (g_conns[i].active && g_conns[i].kind == CONN_LISTEN_ONLY) {
                logmsg("Listen-Only 0x%08x dropped: no producing connection left",
                       g_conns[i].to_conn_id);
                g_conns[i].active = 0;
            }
        }
    }
    if (g_mcast_active && conn_count_mcast_producers() == 0) {
        struct in_addr a = { .s_addr = g_mcast_addr };
        logmsg("Multicast T->O production stopped (%s)", inet_ntoa(a));
        g_mcast_active = 0;
    }
}

/* Build an unsuccessful Forward Open response carrying an extended status. */
static int fo_error(int large, uint16_t ext, uint16_t serial,
                    uint16_t vid, uint32_t osn, uint8_t *resp)
{
    resp[0] = (large ? CIP_LARGE_FORWARD_OPEN : CIP_FORWARD_OPEN) | CIP_REPLY_MASK;
    resp[1] = 0;
    resp[2] = 0x01;        /* general status: connection failure          */
    resp[3] = 1;           /* additional status size = 1 word             */
    uint8_t *r = resp + 4;
    put_u16(r, ext);    r += 2;   /* extended status                      */
    put_u16(r, serial); r += 2;   /* unsuccessful-response fields follow  */
    put_u16(r, vid);    r += 2;
    put_u32(r, osn);    r += 4;
    *r++ = 0;  /* remaining path size */
    *r++ = 0;  /* reserved */
    return (int)(r - resp);
}

/*
 * Parse and service a Forward Open request body (the bytes after the MR
 * request path).  'peer_ip' is the TCP peer (originator) address used as the
 * UDP I/O destination.  Builds the MR response into 'resp'; returns length.
 */
static int do_forward_open(int large, const uint8_t *d, int dl,
                           struct in_addr peer_ip, uint8_t *resp)
{
    const uint8_t *p = d;
    uint8_t  priority_tick = *p++;
    uint8_t  timeout_ticks = *p++;
    (void)priority_tick;
    uint32_t ot_cid = get_u32(p); p += 4;
    uint32_t to_cid = get_u32(p); p += 4;
    uint16_t serial = get_u16(p); p += 2;
    uint16_t vid    = get_u16(p); p += 2;
    uint32_t osn    = get_u32(p); p += 4;
    uint8_t  tmo_mult = *p++;
    p += 3; /* reserved */
    uint32_t ot_rpi = get_u32(p); p += 4;
    uint32_t ot_par = large ? get_u32(p) : get_u16(p); p += large ? 4 : 2;
    uint32_t to_rpi = get_u32(p); p += 4;
    uint32_t to_par = large ? get_u32(p) : get_u16(p); p += large ? 4 : 2;
    uint8_t  ttt    = *p++;                       /* transport type/trigger */
    uint8_t  path_words = *p++;
    epath_t ep;
    parse_epath(p, path_words, &ep);
    (void)dl; (void)ttt; (void)timeout_ticks;

    uint16_t ot_size = conn_param_size(ot_par, large);
    uint16_t to_size = conn_param_size(to_par, large);
    int ot_type = conn_param_type(ot_par, large);
    int to_type = conn_param_type(to_par, large);

    /* ---- classify the application connection kind ----
     * A heartbeat O->T (Null connection type or zero size) means the originator
     * does not own outputs: it is Input-Only, or Listen-Only when it joins the
     * shared (multicast) T->O production. */
    conn_kind_t kind;
    if (ot_type == CT_NULL || ot_size == 0)
        kind = (to_type == CT_MULTICAST) ? CONN_LISTEN_ONLY : CONN_INPUT_ONLY;
    else
        kind = CONN_EXCLUSIVE;

    /* re-open of the same originator triple is allowed */
    conn_t *existing = conn_find_by_serial(serial, vid, osn);

    /* ---- validate against configured policy ---- */
    int allowed = (kind == CONN_EXCLUSIVE   && g_allow_exclusive) ||
                  (kind == CONN_INPUT_ONLY  && g_allow_input_only) ||
                  (kind == CONN_LISTEN_ONLY && g_allow_listen_only);
    if (!allowed) {
        logmsg("Forward Open rejected: %s connections not allowed", kind_name(kind));
        return fo_error(large, CM_EXT_CONN_TYPE_UNSUP, serial, vid, osn, resp);
    }
    if (kind == CONN_EXCLUSIVE && !existing && conn_count_kind(CONN_EXCLUSIVE) > 0) {
        logmsg("Forward Open rejected: ownership conflict (exclusive owner exists)");
        return fo_error(large, CM_EXT_OWNERSHIP, serial, vid, osn, resp);
    }
    if (kind == CONN_LISTEN_ONLY && !g_mcast_active) {
        logmsg("Forward Open rejected: listen-only has no multicast production to join");
        return fo_error(large, CM_EXT_NO_OWNER, serial, vid, osn, resp);
    }
    if (g_rpi_min_us && (to_rpi < g_rpi_min_us ||
                         (kind == CONN_EXCLUSIVE && ot_rpi < g_rpi_min_us))) {
        logmsg("Forward Open rejected: RPI below minimum (%u us)", g_rpi_min_us);
        return fo_error(large, CM_EXT_RPI_NOT_SUPPORTED, serial, vid, osn, resp);
    }
    if (g_rpi_max_us && (to_rpi > g_rpi_max_us ||
                         (kind == CONN_EXCLUSIVE && ot_rpi > g_rpi_max_us))) {
        logmsg("Forward Open rejected: RPI above maximum (%u us)", g_rpi_max_us);
        return fo_error(large, CM_EXT_RPI_NOT_SUPPORTED, serial, vid, osn, resp);
    }
    if (kind == CONN_EXCLUSIVE && g_exp_out_size && ot_size != g_exp_out_size) {
        logmsg("Forward Open rejected: O->T size %u != expected %u", ot_size, g_exp_out_size);
        return fo_error(large, CM_EXT_INVALID_OT_SIZE, serial, vid, osn, resp);
    }
    if (g_exp_in_size && to_size != g_exp_in_size) {
        logmsg("Forward Open rejected: T->O size %u != expected %u", to_size, g_exp_in_size);
        return fo_error(large, CM_EXT_INVALID_TO_SIZE, serial, vid, osn, resp);
    }

    conn_t *c = existing ? existing : conn_alloc();
    if (!c) {
        logmsg("Forward Open rejected: connection limit reached");
        return fo_error(large, CM_EXT_CONN_LIMIT, serial, vid, osn, resp);
    }

    c->active       = 1;
    c->kind         = kind;
    c->ot_conn_id   = ot_cid;
    c->to_conn_id   = (to_cid != 0) ? to_cid : g_next_cid++;
    c->conn_serial  = serial;
    c->vendor_id    = vid;
    c->orig_serial  = osn;
    c->ot_rpi_us    = ot_rpi;
    c->to_rpi_us    = to_rpi;
    c->ot_size      = ot_size > sizeof(g_out_image) ? (uint16_t)sizeof(g_out_image) : ot_size;
    c->to_size      = to_size > 512 ? 512 : to_size;
    c->conn_tmo_mult= tmo_mult;
    c->timeout_us   = g_timeout_us ? g_timeout_us : ot_rpi * tmo_mult_value(tmo_mult);
    c->to_seq32     = 0;
    c->to_seq16     = 0;
    c->to_multicast = 0;
    g_fo_sockaddr_to = 0;

    memset(&c->peer, 0, sizeof(c->peer));
    c->peer.sin_family = AF_INET;
    c->peer.sin_port   = htons(EIP_IO_PORT);
    c->peer.sin_addr   = peer_ip;

    /* ---- shared multicast T->O production ---- */
    if (to_type == CT_MULTICAST) {
        c->to_multicast = 1;
        if (!g_mcast_active) {
            /* established by the first producer (owner/input-only); a
             * listen-only would have been rejected above. */
            g_mcast_active  = 1;
            g_mcast_addr    = cip_mcast_alloc(g_my_ip_be);
            g_mcast_conn_id = g_next_cid++;
            g_mcast_rpi_us  = to_rpi;
            g_mcast_size    = c->to_size;
            g_mcast_seq32   = 0;
            g_mcast_seq16   = 0;
            now_mono(&g_mcast_next_send);
            if (g_io_fd >= 0) {
                struct in_addr ifa = { .s_addr = g_my_ip_be ? g_my_ip_be
                                                            : htonl(INADDR_LOOPBACK) };
                uint8_t loop = 1;
                setsockopt(g_io_fd, IPPROTO_IP, IP_MULTICAST_TTL,  &g_mcast_ttl, 1);
                setsockopt(g_io_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, 1);
                setsockopt(g_io_fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
            }
            struct in_addr a = { .s_addr = g_mcast_addr };
            logmsg("Multicast T->O production established: group %s id=0x%08x size=%u rpi=%uus",
                   inet_ntoa(a), g_mcast_conn_id, g_mcast_size, g_mcast_rpi_us);
        }
        /* consumers share the production's connection id and size */
        c->to_conn_id    = g_mcast_conn_id;
        c->to_size       = g_mcast_size;
        g_fo_sockaddr_to = g_mcast_addr;   /* advertise the group to originator */
    }

    now_mono(&c->next_send);
    c->last_recv = c->next_send;

    logmsg("Forward%s Open [%s]: serial=0x%04x vid=0x%04x O->T id=0x%08x(%uB rpi=%uus) "
           "T->O id=0x%08x(%uB rpi=%uus) cfg=%u out=%u in=%u tmo*=%u",
           large ? " (Large)" : "", kind_name(c->kind), serial, vid,
           c->ot_conn_id, c->ot_size, ot_rpi,
           c->to_conn_id, c->to_size, to_rpi,
           ep.n_points > 0 ? ep.conn_points[0] : 0,
           ep.n_points > 1 ? ep.conn_points[1] : 0,
           ep.n_points > 2 ? ep.conn_points[2] : 0,
           tmo_mult_value(tmo_mult));
    if (c->to_multicast) {
        char peer_s[16], grp_s[16];
        struct in_addr a = { .s_addr = g_mcast_addr };
        snprintf(peer_s, sizeof(peer_s), "%s", inet_ntoa(c->peer.sin_addr));
        snprintf(grp_s,  sizeof(grp_s),  "%s", inet_ntoa(a));
        logmsg("Class 1 I/O: O->T from %s:%d, T->O multicast to %s:%d",
               peer_s, EIP_IO_PORT, grp_s, EIP_IO_PORT);
    } else {
        logmsg("Class 1 I/O endpoint: %s:%d (T->O unicast)",
               inet_ntoa(c->peer.sin_addr), EIP_IO_PORT);
    }

    /* Build success response */
    uint8_t *r = resp + 4;
    put_u32(r, c->ot_conn_id); r += 4;
    put_u32(r, c->to_conn_id); r += 4;
    put_u16(r, serial);        r += 2;
    put_u16(r, vid);           r += 2;
    put_u32(r, osn);           r += 4;
    put_u32(r, ot_rpi);        r += 4;   /* O->T API == requested RPI */
    put_u32(r, to_rpi);        r += 4;   /* T->O API == requested RPI */
    *r++ = 0;  /* application reply size (words) */
    *r++ = 0;  /* reserved */

    resp[0] = (large ? CIP_LARGE_FORWARD_OPEN : CIP_FORWARD_OPEN) | CIP_REPLY_MASK;
    resp[1] = 0;
    resp[2] = CIP_STATUS_SUCCESS;
    resp[3] = 0;
    return (int)(r - resp);
}

static int do_forward_close(const uint8_t *d, int dl, uint8_t *resp) {
    const uint8_t *p = d + 2;          /* skip priority/tick + timeout_ticks */
    uint16_t serial = get_u16(p); p += 2;
    uint16_t vid    = get_u16(p); p += 2;
    uint32_t osn    = get_u32(p); p += 4;
    (void)dl;

    conn_t *c = conn_find_by_serial(serial, vid, osn);
    if (c) {
        logmsg("Forward Close [%s]: serial=0x%04x (O->T id=0x%08x)",
               kind_name(c->kind), serial, c->ot_conn_id);
        c->active = 0;
        conns_cleanup();
    } else {
        logmsg("Forward Close: serial=0x%04x (unknown connection)", serial);
    }

    uint8_t *r = resp + 4;
    put_u16(r, serial); r += 2;
    put_u16(r, vid);    r += 2;
    put_u32(r, osn);    r += 4;
    *r++ = 0; *r++ = 0; /* app reply size + reserved */

    resp[0] = CIP_FORWARD_CLOSE | CIP_REPLY_MASK;
    resp[1] = 0;
    resp[2] = c ? CIP_STATUS_SUCCESS : CIP_STATUS_CONN_FAILURE;
    resp[3] = 0;
    return (int)(r - resp);
}

/*
 * Dispatch a Message Router request (used both by UCMM and connected explicit).
 * Returns MR response length in 'resp'.
 */
static int handle_mr(const uint8_t *mr, int mr_len, struct in_addr peer_ip, uint8_t *resp) {
    if (mr_len < 2) return 0;
    uint8_t service   = mr[0];
    uint8_t path_words = mr[1];
    const uint8_t *path = mr + 2;
    epath_t ep;
    parse_epath(path, path_words, &ep);
    const uint8_t *sdata = path + path_words * 2;
    int sdata_len = mr_len - 2 - path_words * 2;

    if (service == CIP_FORWARD_OPEN)
        return do_forward_open(0, sdata, sdata_len, peer_ip, resp);
    if (service == CIP_LARGE_FORWARD_OPEN)
        return do_forward_open(1, sdata, sdata_len, peer_ip, resp);
    if (service == CIP_FORWARD_CLOSE)
        return do_forward_close(sdata, sdata_len, resp);

    /* attribute may be the 2nd logical segment (instance is 1st conn_point) */
    uint32_t inst = ep.n_points > 0 ? ep.conn_points[0] : 0;
    uint8_t  attr = ep.n_points > 1 ? (uint8_t)ep.conn_points[1] : 0;
    return handle_explicit(service, ep.class_id, inst, attr, sdata, sdata_len, resp);
}

/* -------------------------------------------------------------------------- */
/* Encapsulation layer (TCP/44818)                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    int      fd;
    uint32_t session;
    struct in_addr peer_ip;
    uint8_t  rx[BUF_SIZE];
    int      rx_len;
} tcp_client_t;

static uint32_t g_next_session = 0x00010000;

/* forward declaration: ListIdentity item builder (defined further below) */
static int build_list_identity_item(uint8_t *out);

/* Build a SendRRData response that wraps an MR response in a CPF.  When
 * 'sockaddr_to' is non-zero (network order) a Socket Address Info T->O item
 * (0x8001) advertising that multicast group is appended. */
static int build_send_rr_reply(const uint8_t *mr_resp, int mr_len,
                               uint32_t sockaddr_to, uint8_t *out) {
    uint8_t *p = out;
    put_u32(p, 0);  p += 4;   /* interface handle */
    put_u16(p, 0);  p += 2;   /* timeout */
    put_u16(p, sockaddr_to ? 3 : 2); p += 2;   /* item count */
    put_u16(p, CPF_NULL_ADDRESS); p += 2;   /* address item */
    put_u16(p, 0);                p += 2;
    put_u16(p, CPF_UNCONNECTED_DATA); p += 2;/* data item */
    put_u16(p, (uint16_t)mr_len);     p += 2;
    memcpy(p, mr_resp, mr_len);       p += mr_len;
    if (sockaddr_to) {
        /* Socket Address Info T->O: 16-byte sockaddr_in, BIG-endian fields */
        put_u16(p, CPF_SOCKADDR_TO);  p += 2;
        put_u16(p, 16);               p += 2;
        put_u16be(p, AF_INET);        p += 2;
        put_u16be(p, EIP_IO_PORT);    p += 2;
        memcpy(p, &sockaddr_to, 4);   p += 4;   /* already network order */
        memset(p, 0, 8);              p += 8;
    }
    return (int)(p - out);
}

/* Process one full encapsulation message; returns reply length in 'out'. */
static int process_encap(tcp_client_t *cl, const uint8_t *msg, int msg_len, uint8_t *out) {
    encap_hdr_t h;
    encap_hdr_parse(msg, &h);
    const uint8_t *data = msg + ENCAP_HDR_LEN;
    int data_len = msg_len - ENCAP_HDR_LEN;

    encap_hdr_t rh = h;
    rh.status = ENCAP_STATUS_SUCCESS;
    rh.length = 0;

    uint8_t *body = out + ENCAP_HDR_LEN;
    int body_len = 0;

    switch (h.command) {
    case ENCAP_REGISTER: {
        if (cl->session == 0) cl->session = g_next_session++;
        rh.session = cl->session;
        put_u16(body + 0, 1);   /* protocol version */
        put_u16(body + 2, 0);   /* options flags    */
        body_len = 4;
        logmsg("RegisterSession -> handle 0x%08x", cl->session);
        break;
    }
    case ENCAP_UNREGISTER:
        logmsg("UnRegisterSession 0x%08x", h.session);
        cl->session = 0;
        return -1; /* signal: close connection, no reply */

    case ENCAP_LIST_SERVICES: {
        put_u16(body + 0, 1);                 /* item count */
        uint8_t *it = body + 2;
        put_u16(it + 0, CPF_LIST_SERVICES);   /* item type   */
        put_u16(it + 2, 20);                  /* item length */
        put_u16(it + 4, 1);                   /* encap proto version */
        put_u16(it + 6, 0x0120);              /* caps: CIP-over-TCP + Class0/1-UDP */
        memset(it + 8, 0, 16);
        memcpy(it + 8, "Communications", 14);
        body_len = 2 + 4 + 20;
        logmsg("ListServices");
        break;
    }
    case ENCAP_LIST_IDENTITY: {
        body_len = build_list_identity_item(body);
        logmsg("ListIdentity (TCP)");
        break;
    }
    case ENCAP_SEND_RR_DATA: {
        /* interface handle(4) + timeout(2) + CPF */
        const uint8_t *cpf = data + 6;
        uint16_t item_count = get_u16(cpf); cpf += 2;
        const uint8_t *mr = NULL; int mr_len = 0;
        for (uint16_t i = 0; i < item_count; i++) {
            uint16_t type = get_u16(cpf); cpf += 2;
            uint16_t len  = get_u16(cpf); cpf += 2;
            if (type == CPF_UNCONNECTED_DATA) { mr = cpf; mr_len = len; }
            cpf += len;
        }
        if (mr) {
            hexdump("UCMM req", mr, mr_len);
            uint8_t mr_resp[BUF_SIZE];
            g_fo_sockaddr_to = 0;
            int rl = handle_mr(mr, mr_len, cl->peer_ip, mr_resp);
            body_len = build_send_rr_reply(mr_resp, rl, g_fo_sockaddr_to, body);
        } else {
            rh.status = ENCAP_STATUS_INVALID_LENGTH;
        }
        break;
    }
    case ENCAP_NOP:
        return 0; /* no reply */
    default:
        logmsg("Unsupported encap command 0x%04x", h.command);
        rh.status = ENCAP_STATUS_UNSUPPORTED;
        break;
    }

    rh.length = (uint16_t)body_len;
    encap_hdr_build(out, &rh);
    (void)data_len;
    return ENCAP_HDR_LEN + body_len;
}

/* -------------------------------------------------------------------------- */
/* ListIdentity item (shared by TCP + UDP discovery)                          */
/* -------------------------------------------------------------------------- */

static int build_list_identity_item(uint8_t *out) {
    uint8_t *p = out;
    put_u16(p, 1); p += 2;                 /* item count */
    uint8_t *itlen;
    put_u16(p, CPF_LIST_IDENTITY); p += 2; /* item type */
    itlen = p; p += 2;                     /* item length placeholder */
    uint8_t *start = p;

    put_u16(p, 1); p += 2;                 /* encap protocol version */
    /* socket address (BIG-endian): family, port, addr, 8 zero */
    put_u16be(p, AF_INET);      p += 2;
    put_u16be(p, EIP_TCP_PORT); p += 2;
    memcpy(p, &g_my_ip_be, 4);  p += 4;
    memset(p, 0, 8);            p += 8;
    put_u16(p, g_vendor_id);    p += 2;
    put_u16(p, g_device_type);  p += 2;
    put_u16(p, g_product_code); p += 2;
    *p++ = g_rev_major; *p++ = g_rev_minor;
    put_u16(p, g_dev_status);   p += 2;
    put_u32(p, g_serial);       p += 4;
    p += put_short_string(p, g_product_name);
    *p++ = 0xFF;                           /* state */

    put_u16(itlen, (uint16_t)(p - start));
    return (int)(p - out);
}

/* -------------------------------------------------------------------------- */
/* Class 1 implicit I/O (UDP/2222)                                            */
/* -------------------------------------------------------------------------- */

/* Build one T->O CPF packet (sequenced address + connected data with the
 * shared input image). Returns the packet length. */
static int build_to_packet(uint8_t *pkt, uint32_t conn_id,
                           uint32_t seq32, uint16_t seq16, uint16_t size) {
    uint8_t image[512];
    build_input_image(image, size);

    uint8_t *p = pkt;
    put_u16(p, 2); p += 2;                       /* CPF item count */
    put_u16(p, CPF_SEQUENCED_ADDRESS); p += 2;   /* sequenced address item */
    put_u16(p, 8); p += 2;
    put_u32(p, conn_id); p += 4;
    put_u32(p, seq32);   p += 4;
    put_u16(p, CPF_CONNECTED_DATA); p += 2;      /* connected data item */
    uint8_t *dlen = p; p += 2;
    uint8_t *dstart = p;
    put_u16(p, seq16); p += 2;                   /* class-1 sequence count */
    if (g_to_run_idle) { put_u32(p, 1); p += 4; }/* run/idle: run */
    memcpy(p, image, size); p += size;
    put_u16(dlen, (uint16_t)(p - dstart));
    return (int)(p - pkt);
}

/* Produce one unicast (point-to-point) T->O packet to a connection's peer. */
static void io_produce(int io_fd, conn_t *c) {
    uint8_t pkt[BUF_SIZE];
    g_heartbeat++;
    int n = build_to_packet(pkt, c->to_conn_id, ++c->to_seq32, ++c->to_seq16, c->to_size);
    sendto(io_fd, pkt, (size_t)n, 0, (struct sockaddr *)&c->peer, sizeof(c->peer));
}

/* Produce one multicast T->O packet shared by all multicast consumers. */
static void io_produce_mcast(int io_fd) {
    uint8_t pkt[BUF_SIZE];
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(EIP_IO_PORT);
    dst.sin_addr.s_addr = g_mcast_addr;
    g_heartbeat++;
    int n = build_to_packet(pkt, g_mcast_conn_id, ++g_mcast_seq32, ++g_mcast_seq16, g_mcast_size);
    sendto(io_fd, pkt, (size_t)n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* Handle one received O->T implicit packet. */
static void io_consume(const uint8_t *pkt, int len) {
    const uint8_t *p = pkt;
    if (len < 2) return;
    uint16_t items = get_u16(p); p += 2;
    uint32_t conn_id = 0;
    const uint8_t *data = NULL; int data_len = 0;

    for (uint16_t i = 0; i < items && (p - pkt) + 4 <= len; i++) {
        uint16_t type = get_u16(p); p += 2;
        uint16_t ilen = get_u16(p); p += 2;
        if (type == CPF_SEQUENCED_ADDRESS && ilen >= 8) {
            conn_id = get_u32(p);
        } else if (type == CPF_CONNECTED_ADDRESS && ilen >= 4) {
            conn_id = get_u32(p);
        } else if (type == CPF_CONNECTED_DATA) {
            data = p; data_len = ilen;
        }
        p += ilen;
    }
    if (!data) return;

    conn_t *c = conn_find_by_ot(conn_id);
    if (!c) return; /* unknown / stale connection */

    now_mono(&c->last_recv);    /* any O->T packet is also a keep-alive */

    /* Only the exclusive owner contributes real output data; input-only and
     * listen-only O->T packets are heartbeats and carry no application data. */
    if (c->kind != CONN_EXCLUSIVE) return;

    /* connected data = 16-bit seq count [+ 32-bit run/idle] + payload */
    const uint8_t *d = data; int dl = data_len;
    if (dl >= 2) { d += 2; dl -= 2; }            /* class-1 sequence count */
    if (g_ot_run_idle && dl >= 4) { d += 4; dl -= 4; } /* run/idle header  */

    int n = dl; if (n > (int)sizeof(g_out_image)) n = sizeof(g_out_image);
    if (n < 0) n = 0;
    memcpy(g_out_image, d, n);
    g_out_len = (uint16_t)n;
}

/* -------------------------------------------------------------------------- */
/* socket helpers                                                             */
/* -------------------------------------------------------------------------- */

static int make_tcp_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = g_bind_be; a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}
static int make_udp_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_addr.s_addr = g_bind_be; a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

static void on_signal(int s) { (void)s; g_run = 0; }

/* -------------------------------------------------------------------------- */
/* configuration file (INI-style key = value)                                 */
/* -------------------------------------------------------------------------- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static void set_str(char *dst, size_t n, const char *v) {
    /* strip optional surrounding quotes */
    if (*v == '"') { v++; }
    strncpy(dst, v, n - 1);
    dst[n - 1] = '\0';
    char *q = strchr(dst, '"');
    if (q) *q = '\0';
}

/* Apply a single key=value setting (used by the config file). */
static int set_param(const char *k, const char *v) {
    if      (!strcmp(k, "bind"))         g_bind_be = inet_addr(v);
    else if (!strcmp(k, "ip"))           g_my_ip_be = inet_addr(v);
    else if (!strcmp(k, "out_inst"))     g_out_inst = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "in_inst"))      g_in_inst = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "cfg_inst"))     g_cfg_inst = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "out_size"))     g_exp_out_size = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "in_size"))      g_exp_in_size = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "rpi_min_ms"))   g_rpi_min_us = (uint32_t)strtol(v, NULL, 0) * 1000;
    else if (!strcmp(k, "rpi_max_ms"))   g_rpi_max_us = (uint32_t)strtol(v, NULL, 0) * 1000;
    else if (!strcmp(k, "rpi_ms"))       g_rpi_default_us = (uint32_t)strtol(v, NULL, 0) * 1000;
    else if (!strcmp(k, "timeout_ms"))   g_timeout_us = (uint32_t)strtol(v, NULL, 0) * 1000;
    else if (!strcmp(k, "ot_run_idle"))  g_ot_run_idle = atoi(v);
    else if (!strcmp(k, "to_run_idle"))  g_to_run_idle = atoi(v);
    else if (!strcmp(k, "loopback"))     g_loopback = atoi(v);
    else if (!strcmp(k, "allow_exclusive"))   g_allow_exclusive = atoi(v);
    else if (!strcmp(k, "allow_input_only"))  g_allow_input_only = atoi(v);
    else if (!strcmp(k, "allow_listen_only")) g_allow_listen_only = atoi(v);
    else if (!strcmp(k, "vendor_id"))    g_vendor_id = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "device_type"))  g_device_type = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "product_code")) g_product_code = (uint16_t)strtol(v, NULL, 0);
    else if (!strcmp(k, "serial"))       g_serial = (uint32_t)strtoul(v, NULL, 0);
    else if (!strcmp(k, "rev_major"))    g_rev_major = (uint8_t)atoi(v);
    else if (!strcmp(k, "rev_minor"))    g_rev_minor = (uint8_t)atoi(v);
    else if (!strcmp(k, "product_name")) set_str(g_product_name, sizeof(g_product_name), v);
    else if (!strcmp(k, "vendor_name"))  set_str(g_vendor_name, sizeof(g_vendor_name), v);
    else { fprintf(stderr, "config: unknown key '%s'\n", k); return -1; }
    return 0;
}

static int load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "config: cannot open '%s': %s\n", path, strerror(errno)); return -1; }
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *h = strchr(line, '#'); if (h) *h = '\0';   /* comment */
        h = strchr(line, ';');       if (h) *h = '\0';   /* comment */
        if (line[0] == '[') continue;                    /* ignore [section] */
        char *eq = strchr(line, '=');
        if (!eq) { if (*trim(line)) fprintf(stderr, "config:%d: no '='\n", lineno); continue; }
        *eq = '\0';
        char *key = trim(line), *val = trim(eq + 1);
        if (!*key) continue;
        set_param(key, val);
    }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* EDS (Electronic Data Sheet) generation                                     */
/* -------------------------------------------------------------------------- */

static int write_eds(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "eds: cannot write '%s': %s\n", path, strerror(errno)); return -1; }

    uint16_t out_size = g_exp_out_size ? g_exp_out_size : ASM_OUTPUT_SIZE;
    uint16_t in_size  = g_exp_in_size  ? g_exp_in_size  : ASM_INPUT_SIZE;
    uint32_t rpi_min  = g_rpi_min_us ? g_rpi_min_us : 1000;
    uint32_t rpi_max  = g_rpi_max_us ? g_rpi_max_us : 10000000;
    uint32_t rpi_def  = g_rpi_default_us;
    if (rpi_def < rpi_min) rpi_def = rpi_min;
    if (rpi_def > rpi_max) rpi_def = rpi_max;

    time_t now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    char date[16], tstr[16];
    strftime(date, sizeof(date), "%m-%d-%Y", &tm);
    strftime(tstr, sizeof(tstr), "%H:%M:%S", &tm);

    fprintf(f,
        "$ EtherNet/IP Electronic Data Sheet\n"
        "$ Generated by eip_adapter\n\n"
        "[File]\n"
        "        DescText = \"EDS for %s\";\n"
        "        CreateDate = %s;\n"
        "        CreateTime = %s;\n"
        "        ModDate = %s;\n"
        "        ModTime = %s;\n"
        "        Revision = 1.1;\n\n",
        g_product_name, date, tstr, date, tstr);

    fprintf(f,
        "[Device]\n"
        "        VendCode = %u;\n"
        "        VendName = \"%s\";\n"
        "        ProdType = %u;\n"
        "        ProdTypeStr = \"Communications Adapter\";\n"
        "        ProdCode = %u;\n"
        "        MajRev = %u;\n"
        "        MinRev = %u;\n"
        "        ProdName = \"%s\";\n"
        "        Catalog = \"%s\";\n\n",
        g_vendor_id, g_vendor_name, g_device_type, g_product_code,
        g_rev_major, g_rev_minor, g_product_name, g_product_name);

    fprintf(f,
        "[Device Classification]\n"
        "        Class1 = EtherNetIP;\n\n");

    /* RPI parameters referenced by the connection */
    fprintf(f,
        "[Params]\n"
        "        Param1 =\n"
        "                0,,,                       $ reserved, link path size, link path\n"
        "                0x0000,                    $ descriptor\n"
        "                0xC8,                      $ data type: UDINT\n"
        "                4,                         $ data size (bytes)\n"
        "                \"O2T RPI\",\"microseconds\",\"\",\n"
        "                %u,%u,%u,                  $ min, max, default\n"
        "                ,,,,;\n"
        "        Param2 =\n"
        "                0,,,\n"
        "                0x0000,\n"
        "                0xC8,\n"
        "                4,\n"
        "                \"T2O RPI\",\"microseconds\",\"\",\n"
        "                %u,%u,%u,\n"
        "                ,,,,;\n\n",
        rpi_min, rpi_max, rpi_def, rpi_min, rpi_max, rpi_def);

    /* Assembly object: output (O->T consumed) and input (T->O produced) */
    fprintf(f,
        "[Assembly]\n"
        "        Object_Name = \"Assembly Object\";\n"
        "        Object_Class_Code = 0x04;\n"
        "        Assem%u =\n"
        "                \"Output Data (O2T, consumed)\",,%u,\n"
        "                0x0000,,,;            $ size in BITS\n"
        "        Assem%u =\n"
        "                \"Input Data (T2O, produced)\",,%u,\n"
        "                0x0000,,,;\n"
        "        Assem%u =\n"
        "                \"Configuration\",,0,\n"
        "                0x0000,,,;\n\n",
        g_out_inst, out_size * 8, g_in_inst, in_size * 8, g_cfg_inst);

    /* Connection Manager: one entry per supported application connection kind.
     * Template trigger/transport and param bitmasks (validate in EZ-EDS). */
    fprintf(f, "[Connection Manager]\n");
    int cn = 0;
    if (g_allow_exclusive) {
        fprintf(f,
            "        Connection%d =\n"
            "                0x84010002,           $ Class 1, cyclic, exclusive owner\n"
            "                0x44640405,           $ params: O2T P2P, T2O P2P\n"
            "                ,Param1,%u,,          $ O2T RPI(param), size(bytes), format\n"
            "                ,Param2,%u,,          $ T2O RPI(param), size(bytes), format\n"
            "                ,,,\n"
            "                \"Exclusive Owner\",\"\",\n"
            "                \"20 04 24 %02X 2C %02X 2C %02X\";\n",
            ++cn, out_size, in_size, g_cfg_inst & 0xFF, g_out_inst & 0xFF, g_in_inst & 0xFF);
    }
    if (g_allow_input_only) {
        fprintf(f,
            "        Connection%d =\n"
            "                0x82010002,           $ Class 1, cyclic, input only\n"
            "                0x44240405,           $ params: O2T heartbeat, T2O P2P\n"
            "                ,Param1,0,,           $ O2T heartbeat (size 0)\n"
            "                ,Param2,%u,,          $ T2O RPI(param), size(bytes)\n"
            "                ,,,\n"
            "                \"Input Only\",\"\",\n"
            "                \"20 04 24 %02X 2C %02X 2C %02X\";\n",
            ++cn, in_size, g_cfg_inst & 0xFF, g_out_inst & 0xFF, g_in_inst & 0xFF);
    }
    if (g_allow_listen_only) {
        fprintf(f,
            "        Connection%d =\n"
            "                0x81010002,           $ Class 1, cyclic, listen only\n"
            "                0x46240405,           $ params: O2T heartbeat, T2O multicast\n"
            "                ,Param1,0,,           $ O2T heartbeat (size 0)\n"
            "                ,Param2,%u,,          $ T2O RPI(param), size(bytes)\n"
            "                ,,,\n"
            "                \"Listen Only\",\"\",\n"
            "                \"20 04 24 %02X 2C %02X 2C %02X\";\n",
            ++cn, in_size, g_cfg_inst & 0xFF, g_out_inst & 0xFF, g_in_inst & 0xFF);
    }
    fprintf(f, "\n");

    fclose(f);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

/* long-only option ids */
enum {
    OPT_CFG_INST = 1000, OPT_OUT_SIZE, OPT_IN_SIZE,
    OPT_RPI_MIN, OPT_RPI_MAX, OPT_TIMEOUT,
    OPT_VENDOR, OPT_DEVTYPE, OPT_PRODCODE, OPT_SERIAL, OPT_NAME, OPT_REV,
    OPT_VENDNAME, OPT_RPI_DEF, OPT_WRITE_EDS,
    OPT_NO_EXCLUSIVE, OPT_NO_INPUT_ONLY, OPT_NO_LISTEN_ONLY
};

static void usage(const char *prog) {
    printf("Usage: %s [options]\n"
           "EtherNet/IP adapter with Class 1 implicit I/O.\n\n"
           "Configuration file / EDS:\n"
           "  --config FILE       load settings from an INI-style file (CLI overrides)\n"
           "  --write-eds FILE    generate an EDS from the config and exit\n"
           "\nNetwork / addressing:\n"
           "  --ip A.B.C.D        advertise this IP in ListIdentity\n"
           "  --bind A.B.C.D      bind sockets to this address (default 0.0.0.0)\n"
           "\nAssembly instances:\n"
           "  --out-inst N        output (O->T) assembly instance (default %d)\n"
           "  --in-inst N         input  (T->O) assembly instance (default %d)\n"
           "  --cfg-inst N        configuration assembly instance (default %d)\n"
           "\nClass 1 connection policy (validated against Forward Open):\n"
           "  --out-size N        require O->T (consumed) size = N bytes\n"
           "  --in-size N         require T->O (produced) size = N bytes\n"
           "  --rpi-min-ms N      reject Forward Open with RPI below N ms\n"
           "  --rpi-max-ms N      reject Forward Open with RPI above N ms\n"
           "  --timeout-ms N      override O->T inactivity timeout (else RPI*mult)\n"
           "  --no-exclusive      do not accept Exclusive-Owner connections\n"
           "  --no-input-only     do not accept Input-Only connections\n"
           "  --no-listen-only    do not accept Listen-Only connections\n"
           "\nReal-time format / behaviour:\n"
           "  --no-ot-run-idle    O->T data has NO 32-bit run/idle header\n"
           "  --to-run-idle       prepend 32-bit run/idle header on T->O\n"
           "  --no-loopback       do not mirror O->T into T->O payload\n"
           "\nIdentity object (numbers accept 0x.. hex):\n"
           "  --vendor-id N       vendor id        (default 0x%04x)\n"
           "  --vendor-name STR   vendor name (EDS) (default \"OpenSource\")\n"
           "  --device-type N     device type      (default %u)\n"
           "  --product-code N    product code     (default 0x%04x)\n"
           "  --serial N          serial number    (default 0x%08x)\n"
           "  --rev MAJOR.MINOR   revision         (default %u.%u)\n"
           "  --product-name STR  product name     (default \"%s\")\n"
           "  --rpi-ms N          RPI advertised in the EDS (default 10)\n"
           "\nMisc:\n"
           "  --quiet             reduce logging\n"
           "  --help              this help\n",
           prog, ASM_OUTPUT_INST, ASM_INPUT_INST, ASM_CONFIG_INST,
           DEV_VENDOR_ID, DEV_DEVICE_TYPE, DEV_PRODUCT_CODE, DEV_SERIAL,
           DEV_REV_MAJOR, DEV_REV_MINOR, DEV_PRODUCT_NAME);
}

int main(int argc, char **argv) {
    static struct option opts[] = {
        {"config",         required_argument, 0, 'c'},
        {"write-eds",      required_argument, 0, OPT_WRITE_EDS},
        {"ip",             required_argument, 0, 'i'},
        {"bind",           required_argument, 0, 'b'},
        {"no-ot-run-idle", no_argument,       0, 'r'},
        {"to-run-idle",    no_argument,       0, 't'},
        {"no-loopback",    no_argument,       0, 'l'},
        {"out-inst",       required_argument, 0, 'o'},
        {"in-inst",        required_argument, 0, 'n'},
        {"cfg-inst",       required_argument, 0, OPT_CFG_INST},
        {"out-size",       required_argument, 0, OPT_OUT_SIZE},
        {"in-size",        required_argument, 0, OPT_IN_SIZE},
        {"rpi-min-ms",     required_argument, 0, OPT_RPI_MIN},
        {"rpi-max-ms",     required_argument, 0, OPT_RPI_MAX},
        {"timeout-ms",     required_argument, 0, OPT_TIMEOUT},
        {"vendor-id",      required_argument, 0, OPT_VENDOR},
        {"device-type",    required_argument, 0, OPT_DEVTYPE},
        {"product-code",   required_argument, 0, OPT_PRODCODE},
        {"serial",         required_argument, 0, OPT_SERIAL},
        {"product-name",   required_argument, 0, OPT_NAME},
        {"vendor-name",    required_argument, 0, OPT_VENDNAME},
        {"rev",            required_argument, 0, OPT_REV},
        {"rpi-ms",         required_argument, 0, OPT_RPI_DEF},
        {"no-exclusive",   no_argument,       0, OPT_NO_EXCLUSIVE},
        {"no-input-only",  no_argument,       0, OPT_NO_INPUT_ONLY},
        {"no-listen-only", no_argument,       0, OPT_NO_LISTEN_ONLY},
        {"quiet",          no_argument,       0, 'q'},
        {"help",           no_argument,       0, 'h'},
        {0,0,0,0}
    };

    /* Pre-scan for --config/-c so that command-line flags override the file. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) {
            if (i + 1 < argc) load_config(argv[i + 1]);
        } else if (!strncmp(argv[i], "--config=", 9)) {
            load_config(argv[i] + 9);
        }
    }

    const char *eds_path = NULL;
    int c;
    while ((c = getopt_long(argc, argv, "c:i:b:rtlo:n:qh", opts, NULL)) != -1) {
        switch (c) {
        case 'c': break; /* already handled in the pre-scan */
        case OPT_WRITE_EDS: eds_path = optarg; break;
        case OPT_VENDNAME: set_str(g_vendor_name, sizeof(g_vendor_name), optarg); break;
        case OPT_RPI_DEF:  g_rpi_default_us = (uint32_t)strtol(optarg, NULL, 0) * 1000; break;
        case OPT_NO_EXCLUSIVE:   g_allow_exclusive = 0; break;
        case OPT_NO_INPUT_ONLY:  g_allow_input_only = 0; break;
        case OPT_NO_LISTEN_ONLY: g_allow_listen_only = 0; break;
        case 'i': g_my_ip_be = inet_addr(optarg); break;
        case 'b': g_bind_be  = inet_addr(optarg); break;
        case 'r': g_ot_run_idle = 0; break;
        case 't': g_to_run_idle = 1; break;
        case 'l': g_loopback = 0; break;
        case 'o': g_out_inst = (uint16_t)strtol(optarg, NULL, 0); break;
        case 'n': g_in_inst  = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_CFG_INST: g_cfg_inst = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_OUT_SIZE: g_exp_out_size = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_IN_SIZE:  g_exp_in_size  = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_RPI_MIN:  g_rpi_min_us = (uint32_t)strtol(optarg, NULL, 0) * 1000; break;
        case OPT_RPI_MAX:  g_rpi_max_us = (uint32_t)strtol(optarg, NULL, 0) * 1000; break;
        case OPT_TIMEOUT:  g_timeout_us = (uint32_t)strtol(optarg, NULL, 0) * 1000; break;
        case OPT_VENDOR:   g_vendor_id    = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_DEVTYPE:  g_device_type  = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_PRODCODE: g_product_code = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_SERIAL:   g_serial       = (uint32_t)strtol(optarg, NULL, 0); break;
        case OPT_NAME:
            strncpy(g_product_name, optarg, sizeof(g_product_name) - 1);
            g_product_name[sizeof(g_product_name) - 1] = '\0';
            break;
        case OPT_REV: {
            int mj = 1, mn = 1;
            sscanf(optarg, "%d.%d", &mj, &mn);
            g_rev_major = (uint8_t)mj; g_rev_minor = (uint8_t)mn;
            break;
        }
        case 'q': g_verbose = 0; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* EDS generation is a one-shot action: write and exit. */
    if (eds_path) {
        if (write_eds(eds_path) != 0) return 1;
        printf("EDS written to %s\n", eds_path);
        return 0;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int tcp_fd = make_tcp_listener(EIP_TCP_PORT);
    int disc_fd = make_udp_socket(EIP_UDP_PORT);
    int io_fd  = make_udp_socket(EIP_IO_PORT);
    if (tcp_fd < 0 || disc_fd < 0 || io_fd < 0) {
        fprintf(stderr, "socket bind failed: %s "
                "(need privileges for ports %d/%d?)\n",
                strerror(errno), EIP_TCP_PORT, EIP_IO_PORT);
        return 1;
    }
    g_io_fd = io_fd;   /* used to configure multicast egress on demand */

    logmsg("EtherNet/IP adapter '%s' ready (vendor=0x%04x dev_type=%u "
           "prod=0x%04x rev=%u.%u serial=0x%08x)",
           g_product_name, g_vendor_id, g_device_type, g_product_code,
           g_rev_major, g_rev_minor, g_serial);
    logmsg("  TCP %d (explicit), UDP %d (discovery), UDP %d (Class 1 I/O)",
           EIP_TCP_PORT, EIP_UDP_PORT, EIP_IO_PORT);
    logmsg("  assemblies: O->T(out)=%u T->O(in)=%u config=%u",
           g_out_inst, g_in_inst, g_cfg_inst);
    logmsg("  accepted connections:%s%s%s",
           g_allow_exclusive   ? " Exclusive-Owner" : "",
           g_allow_input_only  ? " Input-Only" : "",
           g_allow_listen_only ? " Listen-Only" : "");
    logmsg("  O->T run/idle=%d  T->O run/idle=%d  loopback=%d",
           g_ot_run_idle, g_to_run_idle, g_loopback);
    if (g_exp_out_size || g_exp_in_size)
        logmsg("  size policy: O->T=%u T->O=%u (Forward Open must match)",
               g_exp_out_size, g_exp_in_size);
    if (g_rpi_min_us || g_rpi_max_us)
        logmsg("  RPI policy: min=%uus max=%uus", g_rpi_min_us, g_rpi_max_us);
    if (g_timeout_us)
        logmsg("  timeout override: %u us", g_timeout_us);

    tcp_client_t clients[MAX_TCP_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) clients[i].fd = -1;

    while (g_run) {
        struct pollfd pfd[3 + MAX_TCP_CLIENTS];
        int nfd = 0;
        pfd[nfd].fd = tcp_fd;  pfd[nfd].events = POLLIN; nfd++;
        pfd[nfd].fd = disc_fd; pfd[nfd].events = POLLIN; nfd++;
        pfd[nfd].fd = io_fd;   pfd[nfd].events = POLLIN; nfd++;
        int cl_base = nfd;
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (clients[i].fd >= 0) { pfd[nfd].fd = clients[i].fd; pfd[nfd].events = POLLIN; nfd++; }
        }

        /* compute poll timeout from the nearest produce deadline */
        int timeout_ms = 1000;
        struct timespec now; now_mono(&now);
        for (int i = 0; i < MAX_CONNS; i++) {
            if (!g_conns[i].active || g_conns[i].to_multicast) continue;
            long us = ts_diff_us(&g_conns[i].next_send, &now);
            int ms = us <= 0 ? 0 : (int)(us / 1000);
            if (ms < timeout_ms) timeout_ms = ms;
        }
        if (g_mcast_active) {
            long us = ts_diff_us(&g_mcast_next_send, &now);
            int ms = us <= 0 ? 0 : (int)(us / 1000);
            if (ms < timeout_ms) timeout_ms = ms;
        }

        int r = poll(pfd, nfd, timeout_ms);
        if (r < 0) { if (errno == EINTR) continue; break; }

        /* --- new TCP connection --- */
        if (pfd[0].revents & POLLIN) {
            struct sockaddr_in pa; socklen_t pl = sizeof(pa);
            int nfd2 = accept(tcp_fd, (struct sockaddr *)&pa, &pl);
            if (nfd2 >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_TCP_CLIENTS; i++) if (clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) { close(nfd2); }
                else {
                    int one = 1; setsockopt(nfd2, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    memset(&clients[slot], 0, sizeof(tcp_client_t));
                    clients[slot].fd = nfd2;
                    clients[slot].peer_ip = pa.sin_addr;
                    if (g_my_ip_be == 0) {
                        /* learn our own IP from the local side of the socket */
                        struct sockaddr_in la; socklen_t ll = sizeof(la);
                        if (getsockname(nfd2, (struct sockaddr *)&la, &ll) == 0)
                            g_my_ip_be = la.sin_addr.s_addr;
                    }
                    logmsg("TCP connect from %s", inet_ntoa(pa.sin_addr));
                }
            }
        }

        /* --- UDP discovery (ListIdentity broadcast) --- */
        if (pfd[1].revents & POLLIN) {
            uint8_t buf[BUF_SIZE];
            struct sockaddr_in pa; socklen_t pl = sizeof(pa);
            int n = recvfrom(disc_fd, buf, sizeof(buf), 0, (struct sockaddr *)&pa, &pl);
            if (n >= ENCAP_HDR_LEN) {
                encap_hdr_t h; encap_hdr_parse(buf, &h);
                if (h.command == ENCAP_LIST_IDENTITY) {
                    uint8_t out[BUF_SIZE];
                    int bl = build_list_identity_item(out + ENCAP_HDR_LEN);
                    encap_hdr_t rh = h; rh.status = 0; rh.length = (uint16_t)bl;
                    encap_hdr_build(out, &rh);
                    sendto(disc_fd, out, ENCAP_HDR_LEN + bl, 0,
                           (struct sockaddr *)&pa, pl);
                    logmsg("ListIdentity (UDP) from %s", inet_ntoa(pa.sin_addr));
                }
            }
        }

        /* --- Class 1 I/O input (O->T) --- */
        if (pfd[2].revents & POLLIN) {
            uint8_t buf[BUF_SIZE];
            struct sockaddr_in pa; socklen_t pl = sizeof(pa);
            int n = recvfrom(io_fd, buf, sizeof(buf), 0, (struct sockaddr *)&pa, &pl);
            if (n > 0) io_consume(buf, n);
        }

        /* --- TCP client data --- */
        for (int i = 0; i < MAX_TCP_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            int idx = -1;
            for (int k = cl_base; k < nfd; k++) if (pfd[k].fd == clients[i].fd) { idx = k; break; }
            if (idx < 0 || !(pfd[idx].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            tcp_client_t *cl = &clients[i];
            int n = recv(cl->fd, cl->rx + cl->rx_len, sizeof(cl->rx) - cl->rx_len, 0);
            if (n <= 0) {
                logmsg("TCP disconnect %s", inet_ntoa(cl->peer_ip));
                close(cl->fd); cl->fd = -1; continue;
            }
            cl->rx_len += n;

            /* process all complete encapsulation messages in the buffer */
            int off = 0, drop = 0;
            while (cl->rx_len - off >= ENCAP_HDR_LEN) {
                uint16_t blen = get_u16(cl->rx + off + 2);
                int total = ENCAP_HDR_LEN + blen;
                if (cl->rx_len - off < total) break; /* wait for more */
                uint8_t out[BUF_SIZE];
                int rl = process_encap(cl, cl->rx + off, total, out);
                if (rl == -1) { drop = 1; off += total; break; }
                if (rl > 0) send(cl->fd, out, rl, 0);
                off += total;
            }
            if (off > 0) {
                memmove(cl->rx, cl->rx + off, cl->rx_len - off);
                cl->rx_len -= off;
            }
            if (drop) { close(cl->fd); cl->fd = -1; }
        }

        /* --- cyclic production + timeout supervision --- */
        now_mono(&now);
        for (int i = 0; i < MAX_CONNS; i++) {
            conn_t *cc = &g_conns[i];
            if (!cc->active) continue;

            if (cc->timeout_us &&
                ts_diff_us(&now, &cc->last_recv) > (long)cc->timeout_us) {
                logmsg("Connection 0x%08x [%s] timed out (no O->T for %u us)",
                       cc->ot_conn_id, kind_name(cc->kind), cc->timeout_us);
                cc->active = 0;
                conns_cleanup();
                continue;
            }
            /* multicast consumers are served by the shared stream below */
            if (cc->to_multicast) continue;
            if (ts_diff_us(&now, &cc->next_send) >= 0) {
                io_produce(io_fd, cc);
                ts_add_us(&cc->next_send, cc->to_rpi_us ? cc->to_rpi_us : 10000);
                /* avoid drift pile-up if we fell behind */
                if (ts_diff_us(&now, &cc->next_send) > 0) {
                    cc->next_send = now;
                    ts_add_us(&cc->next_send, cc->to_rpi_us ? cc->to_rpi_us : 10000);
                }
            }
        }

        /* --- shared multicast T->O production --- */
        if (g_mcast_active && ts_diff_us(&now, &g_mcast_next_send) >= 0) {
            io_produce_mcast(io_fd);
            uint32_t rpi = g_mcast_rpi_us ? g_mcast_rpi_us : 10000;
            ts_add_us(&g_mcast_next_send, rpi);
            if (ts_diff_us(&now, &g_mcast_next_send) > 0) {
                g_mcast_next_send = now;
                ts_add_us(&g_mcast_next_send, rpi);
            }
        }
    }

    logmsg("shutting down");
    for (int i = 0; i < MAX_TCP_CLIENTS; i++) if (clients[i].fd >= 0) close(clients[i].fd);
    close(tcp_fd); close(disc_fd); close(io_fd);
    return 0;
}
