/*
 * device.h - identity and I/O assembly model of the adapter
 */
#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

/* Identity object (class 0x01) attributes */
#define DEV_VENDOR_ID    0x1234          /* test/dev vendor id */
#define DEV_DEVICE_TYPE  0x000C          /* 12 = Communications Adapter */
#define DEV_PRODUCT_CODE 0x0065
#define DEV_REV_MAJOR    1
#define DEV_REV_MINOR    1
#define DEV_STATUS       0x0030          /* owned + configured-ish; informational */
#define DEV_SERIAL       0x00C0FFEE
#define DEV_PRODUCT_NAME "QEMU EIP Adapter"

/* Default Assembly instances (overridable on the command line) */
#define ASM_CONFIG_INST   0x97   /* 151 - configuration */
#define ASM_OUTPUT_INST   0x96   /* 150 - O->T, consumed by adapter */
#define ASM_INPUT_INST    0x64   /* 100 - T->O, produced by adapter */

/* Default I/O sizes in bytes (overridable / negotiated via Forward Open) */
#define ASM_OUTPUT_SIZE   32     /* O->T  consumed bytes */
#define ASM_INPUT_SIZE    32     /* T->O  produced bytes */

#endif /* DEVICE_H */
