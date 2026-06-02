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
#include "serialcfg.h"     // SERIALCFG_KEY_LEN

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

// Per-device serial config view (for /api/serial).
typedef struct {
    bool     connected;
    bool     is_vcp;        // device opened via a VCP driver (real UART)
    uint16_t vid, pid;
    char     serial[SERIALCFG_KEY_LEN];   // USB iSerialNumber ("" if none)
    char     key[SERIALCFG_KEY_LEN];      // resolved device key (serial / VID:PID)
    uint32_t baud;          // effective line coding (display shadow)
    uint8_t  bits, parity, stop;
    uint8_t  lc_source;     // 0 default / 1 nvs / 2 rfc2217
} source_usb_serial_info_t;

void source_usb_get_serial_info(source_usb_serial_info_t *out);

// Apply line coding to the currently-open device and update the display shadow.
// src: 0 default / 1 nvs / 2 rfc2217.  A no-op on the wire for native CDC.
esp_err_t source_usb_apply_line_coding(uint32_t baud, uint8_t bits,
                                       uint8_t parity, uint8_t stop, uint8_t src);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_SOURCE_USB_H
