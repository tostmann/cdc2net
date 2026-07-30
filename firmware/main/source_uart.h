// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_uart.h — transparent hardware-UART source (EUL/TUL onboard radio).
//
// Implements the source_t interface (see source.h) for a stick whose radio
// module hangs off an internal UART rather than the USB-host port: the EUL
// EnOcean (TCM515) / TUL line.  It is a BYTE-TRANSPARENT pipe — UART RX bytes
// go straight to the bridge fanout, bridge TX bytes go straight to the UART.
// No on-device ESP3 framing: the host (FHEM) speaks ESP3 end-to-end through
// the pipe, exactly as if the TCM515 were on a local serial port.  Device
// config (baud) runs through the same per-port serialcfg / RFC2217 machinery
// as source_usb, NOT through an in-band command channel.
//
// On init the source brings the radio up: drive SET low (operational baud),
// pulse RST (active-low), configure the UART at the default/NVS baud, then
// pump transparently.

#ifndef CDC2NET_SOURCE_UART_H
#define CDC2NET_SOURCE_UART_H

#include "source.h"

#ifdef __cplusplus
extern "C" {
#endif

source_t *source_uart_init(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_SOURCE_UART_H
