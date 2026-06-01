// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb.h — generic USB-host CDC-ACM source (the CUL/TUL/EUL stick).
//
// Implements the source_t interface (see source.h): installs the USB host +
// CDC-ACM driver, opens the first CDC-ACM-compliant device, feeds its RX
// bytes into the bridge via source->rx_sink, and sends bridge TX bytes back
// to the device.  No protocol framing — a transparent byte pipe.

#ifndef CDC2NET_SOURCE_USB_H
#define CDC2NET_SOURCE_USB_H

#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

source_t *source_usb_init(void);

typedef struct {
    bool     connected;
    uint16_t vid;
    uint16_t pid;
    char     manuf[48];     // USB iManufacturer string
    char     product[48];   // USB iProduct string
    uint32_t rx_bytes;
    uint32_t tx_bytes;
} source_usb_stats_t;

void source_usb_get_stats(source_usb_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_SOURCE_USB_H
