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

// Construct the USB-host CDC-ACM source.  Everything else (stats, serial-info,
// line-coding apply) is reached through the source_t vtable (see source.h), so
// webui.c / main.c stay source-agnostic.
source_t *source_usb_init(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_SOURCE_USB_H
