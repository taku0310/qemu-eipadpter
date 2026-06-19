/*
 * eip.h - EtherNet/IP adapter common definitions
 *
 * EtherNet/IP encapsulation + CIP constants and small endian helpers.
 * All on-the-wire integers in EtherNet/IP encapsulation and CIP are
 * little-endian, EXCEPT the sockaddr items inside CPF which are big-endian.
 */
#ifndef EIP_H
#define EIP_H

#include <stdint.h>
#include <string.h>

/* ---- Well known ports ---- */
#define EIP_TCP_PORT 44818
#define EIP_UDP_PORT 44818
#define EIP_IO_PORT  2222

/* ---- Encapsulation commands ---- */
#define ENCAP_NOP             0x0000
#define ENCAP_LIST_SERVICES   0x0004
#define ENCAP_LIST_IDENTITY   0x0063
#define ENCAP_LIST_INTERFACES 0x0064
#define ENCAP_REGISTER        0x0065
#define ENCAP_UNREGISTER      0x0066
#define ENCAP_SEND_RR_DATA    0x006F
#define ENCAP_SEND_UNIT_DATA  0x0070

/* ---- Encapsulation status ---- */
#define ENCAP_STATUS_SUCCESS          0x0000
#define ENCAP_STATUS_UNSUPPORTED      0x0001
#define ENCAP_STATUS_INVALID_LENGTH   0x0065

/* ---- CPF (Common Packet Format) item type IDs ---- */
#define CPF_NULL_ADDRESS      0x0000
#define CPF_CONNECTED_ADDRESS 0x00A1
#define CPF_CONNECTED_DATA    0x00B1
#define CPF_UNCONNECTED_DATA  0x00B2
#define CPF_LIST_SERVICES     0x0100
#define CPF_LIST_IDENTITY     0x000C
#define CPF_SOCKADDR_OT       0x8000
#define CPF_SOCKADDR_TO       0x8001
#define CPF_SEQUENCED_ADDRESS 0x8002

/* ---- CIP services ---- */
#define CIP_GET_ATTR_ALL      0x01
#define CIP_GET_ATTR_SINGLE   0x0E
#define CIP_FORWARD_CLOSE     0x4E
#define CIP_FORWARD_OPEN      0x54
#define CIP_LARGE_FORWARD_OPEN 0x5B
#define CIP_REPLY_MASK        0x80

/* ---- CIP general status ---- */
#define CIP_STATUS_SUCCESS              0x00
#define CIP_STATUS_PATH_DEST_UNKNOWN    0x05
#define CIP_STATUS_SERVICE_UNSUPPORTED  0x08
#define CIP_STATUS_ATTR_UNSUPPORTED     0x14
#define CIP_STATUS_CONN_FAILURE         0x01

/* ---- CIP class IDs ---- */
#define CLASS_IDENTITY        0x01
#define CLASS_MESSAGE_ROUTER  0x02
#define CLASS_ASSEMBLY        0x04
#define CLASS_CONN_MANAGER    0x06
#define CLASS_TCPIP           0xF5
#define CLASS_ETHERNET_LINK   0xF6

/* ---- little-endian accessors ---- */
static inline void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* big-endian (for CPF sockaddr items) */
static inline void put_u16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)(v & 0xFF);
}
static inline void put_u32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >> 8) & 0xFF);
    p[3] = (uint8_t)(v & 0xFF);
}

/* ---- Encapsulation header (24 bytes) ---- */
#define ENCAP_HDR_LEN 24
typedef struct {
    uint16_t command;
    uint16_t length;        /* length of data following the 24-byte header */
    uint32_t session;
    uint32_t status;
    uint8_t  context[8];
    uint32_t options;
} encap_hdr_t;

static inline void encap_hdr_parse(const uint8_t *b, encap_hdr_t *h) {
    h->command = get_u16(b + 0);
    h->length  = get_u16(b + 2);
    h->session = get_u32(b + 4);
    h->status  = get_u32(b + 8);
    memcpy(h->context, b + 12, 8);
    h->options = get_u32(b + 20);
}
static inline void encap_hdr_build(uint8_t *b, const encap_hdr_t *h) {
    put_u16(b + 0, h->command);
    put_u16(b + 2, h->length);
    put_u32(b + 4, h->session);
    put_u32(b + 8, h->status);
    memcpy(b + 12, h->context, 8);
    put_u32(b + 20, h->options);
}

#endif /* EIP_H */
