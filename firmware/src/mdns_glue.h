// SPDX-License-Identifier: GPL-2.0-or-later
//
// mdns_glue.h — minimal mDNS responder.
//
// Publishes only the device HOSTNAME so it is reachable as
// `cdc2net-XXXX.local` (+ short alias `cdc2net.local`) without knowing the
// DHCP IP.  The raw-TCP stream port is intentionally NOT advertised as a
// service: what's plugged into the OTG port is module-dependent, so a
// protocol-named service (_culfw._tcp etc.) would assert an identity the
// bridge doesn't actually know.  `_http._tcp` will be added with the WebUI.

#ifndef CDC2NET_MDNS_GLUE_H
#define CDC2NET_MDNS_GLUE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Idempotent.  Spawns a task that waits for STA-got-IP, then starts the
// mDNS responder and sets the hostname.  No service records.
esp_err_t mdns_glue_init(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_MDNS_GLUE_H
