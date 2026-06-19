/*
 * eip_scanner.c - EtherNet/IP scanner (CIP originator) with Class 1 implicit I/O
 *
 * Opens a Class 1 (cyclic, implicit) connection to an EtherNet/IP adapter
 * (target) and exchanges real-time I/O:
 *
 *     O->T  (originator -> target) : the scanner PRODUCES output data
 *     T->O  (target -> originator) : the scanner CONSUMES input data
 *
 * Flow: RegisterSession -> Forward Open -> cyclic UDP/2222 I/O -> Forward Close.
 * Supports exclusive-owner, input-only and listen-only connections, and both
 * point-to-point and multicast T->O (joins the group advertised by the target).
 *
 * Runs as an ordinary userspace program on a Linux/Ubuntu host. Pairs with the
 * eip_adapter in this repository.
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include "eip.h"

#define BUF_SIZE 2048

/* connection type codes in network connection parameters */
#define CT_NULL      0
#define CT_MULTICAST 1
#define CT_P2P       2

static volatile sig_atomic_t g_run = 1;
static int g_verbose = 1;

static void logmsg(const char *fmt, ...) {
    if (!g_verbose) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm; localtime_r(&ts.tv_sec, &tm);
    char t[16]; strftime(t, sizeof(t), "%H:%M:%S", &tm);
    fprintf(stderr, "[%s.%03ld] ", t, ts.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int s) { (void)s; g_run = 0; }

/* monotonic time helpers */
static void now_mono(struct timespec *ts) { clock_gettime(CLOCK_MONOTONIC, ts); }
static long ts_diff_us(const struct timespec *a, const struct timespec *b) {
    return (a->tv_sec - b->tv_sec) * 1000000L + (a->tv_nsec - b->tv_nsec) / 1000;
}
static void ts_add_us(struct timespec *ts, uint32_t us) {
    ts->tv_nsec += (long)(us % 1000000) * 1000;
    ts->tv_sec  += us / 1000000;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_nsec -= 1000000000L; ts->tv_sec += 1; }
}

/* -------------------------------------------------------------------------- */
/* encapsulation helpers                                                      */
/* -------------------------------------------------------------------------- */

static int send_encap(int fd, uint16_t cmd, uint32_t session,
                      const uint8_t *body, int body_len) {
    uint8_t buf[BUF_SIZE];
    encap_hdr_t h = {0};
    h.command = cmd; h.length = (uint16_t)body_len; h.session = session;
    encap_hdr_build(buf, &h);
    if (body_len) memcpy(buf + ENCAP_HDR_LEN, body, body_len);
    return send(fd, buf, ENCAP_HDR_LEN + body_len, 0);
}

/* Read one full encapsulation reply (header + body) into buf. */
static int recv_encap(int fd, uint8_t *buf, int cap, encap_hdr_t *h) {
    int got = 0;
    while (got < ENCAP_HDR_LEN) {
        int n = recv(fd, buf + got, ENCAP_HDR_LEN - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    encap_hdr_parse(buf, h);
    int total = ENCAP_HDR_LEN + h->length;
    if (total > cap) return -1;
    while (got < total) {
        int n = recv(fd, buf + got, total - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return total;
}

/* -------------------------------------------------------------------------- */
/* scanner configuration                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *target;      /* adapter IP */
    const char *local;       /* local source IP (single-host testing) */
    const char *mcast_if;    /* interface IP to join multicast on */
    int      conn_type;      /* 0 exclusive, 1 input-only, 2 listen-only */
    int      to_multicast;   /* request multicast T->O */
    uint16_t ot_size;        /* produced O->T bytes */
    uint16_t to_size;        /* consumed T->O bytes */
    uint32_t rpi_us;
    uint16_t out_inst, in_inst, cfg_inst;
    int      ot_run_idle;    /* include 32-bit run/idle header on O->T */
    int      to_run_idle;    /* expect 32-bit run/idle header on T->O */
    uint16_t serial, vendor_id;
    uint32_t osn, ot_cid;
    double   seconds;        /* run duration, 0 = forever */
} cfg_t;

/* Build the Forward Open request body (MR + CPF) and send it. */
static int send_forward_open(int fd, uint32_t session, const cfg_t *c) {
    uint8_t mr[128];
    uint8_t *p = mr;
    *p++ = CIP_FORWARD_OPEN;
    *p++ = 0x02;                              /* path size (words) */
    *p++ = 0x20; *p++ = CLASS_CONN_MANAGER;   /* class 6 */
    *p++ = 0x24; *p++ = 0x01;                 /* instance 1 */

    int ot_type = (c->conn_type == 0) ? CT_P2P : CT_NULL;  /* heartbeat if not owner */
    uint16_t ot_size = (c->conn_type == 0) ? c->ot_size : 0;
    int to_type = c->to_multicast ? CT_MULTICAST : CT_P2P;
    if (c->conn_type == 2) to_type = CT_MULTICAST;          /* listen-only multicast */
    uint16_t ot_par = (uint16_t)((ot_type << 13) | (ot_size & 0x1FF));
    uint16_t to_par = (uint16_t)((to_type << 13) | (c->to_size & 0x1FF));

    *p++ = 0x06; *p++ = 0x9A;                 /* priority/tick, timeout ticks */
    put_u32(p, c->ot_cid); p += 4;            /* O->T conn id (we choose) */
    put_u32(p, 0);         p += 4;            /* T->O conn id (target assigns) */
    put_u16(p, c->serial); p += 2;
    put_u16(p, c->vendor_id); p += 2;
    put_u32(p, c->osn);    p += 4;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0;   /* timeout mult + 3 reserved */
    put_u32(p, c->rpi_us); p += 4;            /* O->T RPI */
    put_u16(p, ot_par);    p += 2;
    put_u32(p, c->rpi_us); p += 4;            /* T->O RPI */
    put_u16(p, to_par);    p += 2;
    *p++ = 0x01;                              /* transport: class 1, cyclic */
    *p++ = 0x04;                              /* connection path size (words) */
    *p++ = 0x20; *p++ = CLASS_ASSEMBLY;
    *p++ = 0x24; *p++ = (uint8_t)c->cfg_inst;
    *p++ = 0x2C; *p++ = (uint8_t)c->out_inst;
    *p++ = 0x2C; *p++ = (uint8_t)c->in_inst;
    int mr_len = (int)(p - mr);

    uint8_t body[256];
    uint8_t *b = body;
    put_u32(b, 0); b += 4;                    /* interface handle */
    put_u16(b, 0); b += 2;                    /* timeout */
    put_u16(b, 2); b += 2;                    /* CPF item count */
    put_u16(b, CPF_NULL_ADDRESS);   b += 2;
    put_u16(b, 0);                  b += 2;
    put_u16(b, CPF_UNCONNECTED_DATA); b += 2;
    put_u16(b, (uint16_t)mr_len);   b += 2;
    memcpy(b, mr, mr_len);          b += mr_len;
    return send_encap(fd, ENCAP_SEND_RR_DATA, session, body, (int)(b - body));
}

/* Parse the Forward Open reply: extract O->T id, T->O id, multicast addr. */
static int parse_forward_open_reply(const uint8_t *body, int len,
                                    uint32_t *ot_id, uint32_t *to_id,
                                    uint32_t *mcast_be) {
    *mcast_be = 0;
    if (len < 8) return -1;
    const uint8_t *p = body + 6;              /* skip iface handle + timeout */
    const uint8_t *end = body + len;
    uint16_t count = get_u16(p); p += 2;
    const uint8_t *mr = NULL; int mr_len = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (p + 4 > end) break;
        uint16_t t = get_u16(p); p += 2;
        uint16_t l = get_u16(p); p += 2;
        if (p + l > end) l = (uint16_t)(end - p);   /* clamp to received reply */
        if (t == CPF_UNCONNECTED_DATA) { mr = p; mr_len = l; }
        else if (t == CPF_SOCKADDR_TO && l >= 8) memcpy(mcast_be, p + 4, 4);
        p += l;
    }
    if (!mr || mr_len < 4) return -1;
    uint8_t gstatus = mr[2], addl = mr[3];
    if (gstatus != 0) {
        uint16_t ext = (addl >= 1 && mr_len >= 6) ? get_u16(mr + 4) : 0;
        logmsg("Forward Open rejected: CIP status=0x%02x extended=0x%04x", gstatus, ext);
        return -1;
    }
    if (mr_len < 12) return -1;               /* need O->T + T->O connection ids */
    *ot_id = get_u32(mr + 4);
    *to_id = get_u32(mr + 8);
    return 0;
}

/* Send a Forward Close. */
static void send_forward_close(int fd, uint32_t session, const cfg_t *c) {
    uint8_t mr[64]; uint8_t *p = mr;
    *p++ = CIP_FORWARD_CLOSE;
    *p++ = 0x02; *p++ = 0x20; *p++ = CLASS_CONN_MANAGER; *p++ = 0x24; *p++ = 0x01;
    *p++ = 0x06; *p++ = 0x9A;
    put_u16(p, c->serial); p += 2;
    put_u16(p, c->vendor_id); p += 2;
    put_u32(p, c->osn); p += 4;
    *p++ = 0x04; *p++ = 0x00;                 /* path size + reserved */
    *p++ = 0x20; *p++ = CLASS_ASSEMBLY;
    *p++ = 0x24; *p++ = (uint8_t)c->cfg_inst;
    *p++ = 0x2C; *p++ = (uint8_t)c->out_inst;
    *p++ = 0x2C; *p++ = (uint8_t)c->in_inst;
    int mr_len = (int)(p - mr);

    uint8_t body[128]; uint8_t *b = body;
    put_u32(b, 0); b += 4; put_u16(b, 0); b += 2; put_u16(b, 2); b += 2;
    put_u16(b, CPF_NULL_ADDRESS);     b += 2; put_u16(b, 0); b += 2;
    put_u16(b, CPF_UNCONNECTED_DATA); b += 2; put_u16(b, (uint16_t)mr_len); b += 2;
    memcpy(b, mr, mr_len); b += mr_len;
    send_encap(fd, ENCAP_SEND_RR_DATA, session, body, (int)(b - body));
}

/* -------------------------------------------------------------------------- */
/* Class 1 I/O                                                                */
/* -------------------------------------------------------------------------- */

/* Build one O->T packet; payload carries a counter + 0xAA fill (heartbeat when
 * size is 0). Returns packet length. */
static int build_ot_packet(uint8_t *pkt, uint32_t conn_id, uint32_t seq32,
                           uint16_t seq16, uint16_t size, uint32_t counter,
                           int run_idle) {
    uint8_t *p = pkt;
    put_u16(p, 2); p += 2;
    put_u16(p, CPF_SEQUENCED_ADDRESS); p += 2;
    put_u16(p, 8); p += 2;
    put_u32(p, conn_id); p += 4;
    put_u32(p, seq32);   p += 4;
    put_u16(p, CPF_CONNECTED_DATA); p += 2;
    uint8_t *dlen = p; p += 2;
    uint8_t *dstart = p;
    put_u16(p, seq16); p += 2;
    if (size > 0) {
        if (run_idle) { put_u32(p, 1); p += 4; }
        put_u32(p, counter); p += 4;
        for (int i = 4; i < size; i++) *p++ = 0xAA;
    }
    put_u16(dlen, (uint16_t)(p - dstart));
    return (int)(p - pkt);
}

/* Parse a received T->O packet: returns consumed-data pointer/len after the
 * sequence count (and optional run/idle header). */
static const uint8_t *parse_to_packet(const uint8_t *pkt, int len, int run_idle,
                                      uint32_t *conn_id, int *out_len) {
    const uint8_t *p = pkt;
    if (len < 2) return NULL;
    uint16_t items = get_u16(p); p += 2;
    const uint8_t *data = NULL; int dl = 0;
    for (uint16_t i = 0; i < items && (p - pkt) + 4 <= len; i++) {
        uint16_t t = get_u16(p); p += 2;
        uint16_t l = get_u16(p); p += 2;
        int avail = len - (int)(p - pkt);
        if (l > avail) l = (uint16_t)avail;       /* never read past the packet */
        if (t == CPF_SEQUENCED_ADDRESS && l >= 8) *conn_id = get_u32(p);
        else if (t == CPF_CONNECTED_DATA) { data = p; dl = l; }
        p += l;
    }
    if (!data) return NULL;
    if (dl >= 2) { data += 2; dl -= 2; }              /* seq count */
    if (run_idle && dl >= 4) { data += 4; dl -= 4; }  /* run/idle header */
    *out_len = dl;
    return data;
}

/* -------------------------------------------------------------------------- */
/* socket setup                                                               */
/* -------------------------------------------------------------------------- */

static int connect_tcp(const cfg_t *c) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (c->local) {
        struct sockaddr_in la = {0};
        la.sin_family = AF_INET; la.sin_addr.s_addr = inet_addr(c->local);
        bind(fd, (struct sockaddr *)&la, sizeof(la));
    }
    struct sockaddr_in ta = {0};
    ta.sin_family = AF_INET; ta.sin_port = htons(EIP_TCP_PORT);
    ta.sin_addr.s_addr = inet_addr(c->target);
    if (connect(fd, (struct sockaddr *)&ta, sizeof(ta)) < 0) { close(fd); return -1; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

enum { OPT_OUT_INST = 1000, OPT_IN_INST, OPT_CFG_INST, OPT_NO_OT_RI,
       OPT_TO_RI, OPT_SERIAL, OPT_OSN, OPT_OTCID, OPT_VENDOR, OPT_MCAST_IF };

static void usage(const char *prog) {
    printf("Usage: %s --target A.B.C.D [options]\n"
           "EtherNet/IP scanner (originator) with Class 1 implicit I/O.\n\n"
           "  --target IP        adapter (target) IP                 [required]\n"
           "  --local IP         local source IP (single-host test)\n"
           "  --conn-type T      exclusive | input-only | listen-only [exclusive]\n"
           "  --to-multicast     request multicast T->O\n"
           "  --mcast-if IP      interface to join the group on       [= --local or 0.0.0.0]\n"
           "  --ot-size N        O->T (produced) bytes                [32]\n"
           "  --to-size N        T->O (consumed) bytes                [32]\n"
           "  --rpi-ms N         requested packet interval (ms)       [50]\n"
           "  --out-inst N       O->T assembly instance               [150]\n"
           "  --in-inst N        T->O assembly instance               [100]\n"
           "  --cfg-inst N       configuration assembly instance      [151]\n"
           "  --no-ot-run-idle   omit the 32-bit run/idle header on O->T\n"
           "  --to-run-idle      expect a 32-bit run/idle header on T->O\n"
           "  --serial N         connection serial number             [0x0001]\n"
           "  --vendor-id N      originator vendor id                 [0x00FF]\n"
           "  --osn N            originator serial number             [0x12345678]\n"
           "  --ot-cid N         O->T connection id we assign         [0x12340001]\n"
           "  --seconds N        run duration, 0 = forever            [0]\n"
           "  --quiet / --help\n", prog);
}

int main(int argc, char **argv) {
    cfg_t c = {
        .target = NULL, .local = NULL, .mcast_if = NULL,
        .conn_type = 0, .to_multicast = 0,
        .ot_size = 32, .to_size = 32, .rpi_us = 50000,
        .out_inst = 150, .in_inst = 100, .cfg_inst = 151,
        .ot_run_idle = 1, .to_run_idle = 0,
        .serial = 0x0001, .vendor_id = 0x00FF, .osn = 0x12345678,
        .ot_cid = 0x12340001, .seconds = 0,
    };

    static struct option opts[] = {
        {"target",       required_argument, 0, 't'},
        {"local",        required_argument, 0, 'l'},
        {"conn-type",    required_argument, 0, 'c'},
        {"to-multicast", no_argument,       0, 'm'},
        {"mcast-if",     required_argument, 0, OPT_MCAST_IF},
        {"ot-size",      required_argument, 0, 'o'},
        {"to-size",      required_argument, 0, 'i'},
        {"rpi-ms",       required_argument, 0, 'r'},
        {"out-inst",     required_argument, 0, OPT_OUT_INST},
        {"in-inst",      required_argument, 0, OPT_IN_INST},
        {"cfg-inst",     required_argument, 0, OPT_CFG_INST},
        {"no-ot-run-idle", no_argument,     0, OPT_NO_OT_RI},
        {"to-run-idle",  no_argument,       0, OPT_TO_RI},
        {"serial",       required_argument, 0, OPT_SERIAL},
        {"vendor-id",    required_argument, 0, OPT_VENDOR},
        {"osn",          required_argument, 0, OPT_OSN},
        {"ot-cid",       required_argument, 0, OPT_OTCID},
        {"seconds",      required_argument, 0, 's'},
        {"quiet",        no_argument,       0, 'q'},
        {"help",         no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "t:l:c:mo:i:r:s:qh", opts, NULL)) != -1) {
        switch (opt) {
        case 't': c.target = optarg; break;
        case 'l': c.local = optarg; break;
        case 'c':
            if      (!strcmp(optarg, "exclusive"))   c.conn_type = 0;
            else if (!strcmp(optarg, "input-only"))  c.conn_type = 1;
            else if (!strcmp(optarg, "listen-only")) c.conn_type = 2;
            else { fprintf(stderr, "bad --conn-type\n"); return 1; }
            break;
        case 'm': c.to_multicast = 1; break;
        case OPT_MCAST_IF: c.mcast_if = optarg; break;
        case 'o': c.ot_size = (uint16_t)strtol(optarg, NULL, 0); break;
        case 'i': c.to_size = (uint16_t)strtol(optarg, NULL, 0); break;
        case 'r': c.rpi_us = (uint32_t)strtol(optarg, NULL, 0) * 1000; break;
        case OPT_OUT_INST: c.out_inst = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_IN_INST:  c.in_inst  = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_CFG_INST: c.cfg_inst = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_NO_OT_RI: c.ot_run_idle = 0; break;
        case OPT_TO_RI:    c.to_run_idle = 1; break;
        case OPT_SERIAL:   c.serial = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_VENDOR:   c.vendor_id = (uint16_t)strtol(optarg, NULL, 0); break;
        case OPT_OSN:      c.osn = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_OTCID:    c.ot_cid = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 's': c.seconds = atof(optarg); break;
        case 'q': g_verbose = 0; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (!c.target) { usage(argv[0]); return 1; }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const char *kind = c.conn_type == 0 ? "exclusive" :
                       c.conn_type == 1 ? "input-only" : "listen-only";
    int heartbeat_only = (c.conn_type != 0);

    /* --- TCP session + Forward Open --- */
    int tcp = connect_tcp(&c);
    if (tcp < 0) { fprintf(stderr, "TCP connect to %s failed: %s\n", c.target, strerror(errno)); return 1; }

    uint8_t buf[BUF_SIZE]; encap_hdr_t h;
    uint8_t reg[4]; put_u16(reg, 1); put_u16(reg + 2, 0);
    send_encap(tcp, ENCAP_REGISTER, 0, reg, 4);
    if (recv_encap(tcp, buf, sizeof(buf), &h) < 0 || h.status != 0) {
        fprintf(stderr, "RegisterSession failed\n"); return 1;
    }
    uint32_t session = h.session;
    logmsg("RegisterSession OK, handle=0x%08x", session);

    send_forward_open(tcp, session, &c);
    int n = recv_encap(tcp, buf, sizeof(buf), &h);
    if (n < 0 || h.status != 0) { fprintf(stderr, "SendRRData(ForwardOpen) failed\n"); return 1; }
    uint32_t ot_id = 0, to_id = 0, mcast_be = 0;
    if (parse_forward_open_reply(buf + ENCAP_HDR_LEN, h.length, &ot_id, &to_id, &mcast_be) < 0)
        return 1;
    char grp[16] = "";
    if (mcast_be) { struct in_addr a = { .s_addr = mcast_be }; snprintf(grp, sizeof(grp), "%s", inet_ntoa(a)); }
    logmsg("Forward Open OK [%s]: O->T id=0x%08x  T->O id=0x%08x%s%s",
           kind, ot_id, to_id, mcast_be ? "  T->O multicast=" : "", grp);

    /* --- UDP sockets: tx (O->T) and rx (T->O) --- */
    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1; setsockopt(tx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET; la.sin_port = htons(EIP_IO_PORT);
    la.sin_addr.s_addr = c.local ? inet_addr(c.local) : INADDR_ANY;
    bind(tx, (struct sockaddr *)&la, sizeof(la));

    int rx;
    if (mcast_be) {
        rx = socket(AF_INET, SOCK_DGRAM, 0);
        setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(rx, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        struct sockaddr_in ga = {0};
        ga.sin_family = AF_INET; ga.sin_port = htons(EIP_IO_PORT);
        ga.sin_addr.s_addr = mcast_be;                /* bind to the group addr */
        bind(rx, (struct sockaddr *)&ga, sizeof(ga));
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = mcast_be;
        mreq.imr_interface.s_addr = c.mcast_if ? inet_addr(c.mcast_if) :
                                    (c.local ? inet_addr(c.local) : INADDR_ANY);
        if (setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
            logmsg("warning: IP_ADD_MEMBERSHIP failed: %s", strerror(errno));
    } else {
        rx = tx;                                      /* unicast: receive on tx socket */
    }

    struct sockaddr_in target_io = {0};
    target_io.sin_family = AF_INET; target_io.sin_port = htons(EIP_IO_PORT);
    target_io.sin_addr.s_addr = inet_addr(c.target);

    /* --- cyclic I/O loop --- */
    uint32_t tx_seq32 = 0, out_counter = 0; uint16_t tx_seq16 = 0;
    uint64_t tx_count = 0, rx_count = 0; uint32_t last_hb = 0;
    struct timespec start, next_tx, next_report;
    now_mono(&start); next_tx = start; next_report = start; ts_add_us(&next_report, 1000000);

    logmsg("Class 1 I/O started (RPI %u ms, O->T %uB, T->O %uB)%s",
           c.rpi_us / 1000, heartbeat_only ? 0 : c.ot_size, c.to_size,
           mcast_be ? ", T->O via multicast" : "");

    while (g_run) {
        struct timespec now; now_mono(&now);
        if (c.seconds > 0 && ts_diff_us(&now, &start) >= (long)(c.seconds * 1e6)) break;

        if (ts_diff_us(&now, &next_tx) >= 0) {
            uint8_t pkt[BUF_SIZE];
            tx_seq32++; tx_seq16++;
            uint16_t sz = heartbeat_only ? 0 : c.ot_size;
            if (!heartbeat_only) out_counter++;
            int len = build_ot_packet(pkt, ot_id, tx_seq32, tx_seq16, sz, out_counter,
                                      c.ot_run_idle);
            sendto(tx, pkt, len, 0, (struct sockaddr *)&target_io, sizeof(target_io));
            tx_count++;
            ts_add_us(&next_tx, c.rpi_us);
            if (ts_diff_us(&now, &next_tx) > 0) { next_tx = now; ts_add_us(&next_tx, c.rpi_us); }
        }

        struct pollfd pfd = { .fd = rx, .events = POLLIN };
        long wait_us = ts_diff_us(&next_tx, &now);
        int r = poll(&pfd, 1, wait_us <= 0 ? 0 : (int)(wait_us / 1000) + 1);
        if (r > 0 && (pfd.revents & POLLIN)) {
            uint8_t rb[BUF_SIZE];
            int m = recv(rx, rb, sizeof(rb), 0);
            if (m > 0) {
                uint32_t cid = 0; int dl = 0;
                const uint8_t *d = parse_to_packet(rb, m, c.to_run_idle, &cid, &dl);
                if (d && cid == to_id) {
                    rx_count++;
                    if (dl >= 4) last_hb = get_u32(d);
                }
            }
        }

        now_mono(&now);
        if (ts_diff_us(&now, &next_report) >= 0) {
            logmsg("status: O->T sent=%llu  T->O recv=%llu  last input[0:4]=%u",
                   (unsigned long long)tx_count, (unsigned long long)rx_count, last_hb);
            ts_add_us(&next_report, 1000000);
        }
    }

    logmsg("closing: Forward Close + UnRegisterSession (sent %llu, recv %llu)",
           (unsigned long long)tx_count, (unsigned long long)rx_count);
    send_forward_close(tcp, session, &c);
    recv_encap(tcp, buf, sizeof(buf), &h);            /* drain reply (best effort) */
    send_encap(tcp, ENCAP_UNREGISTER, session, NULL, 0);

    if (rx != tx) close(rx);
    close(tx);
    close(tcp);
    return 0;
}
