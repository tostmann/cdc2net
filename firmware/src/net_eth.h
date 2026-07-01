// SPDX-License-Identifier: GPL-2.0-or-later
//
// net_eth.h — W5500 SPI Ethernet bring-up (line-wide; FPC add-on on the
// EUL/TUL C6 generation).  Coexists with the WiFi-STA/SoftAP net layer: both
// netifs come up independently and the TCP listener binds INADDR_ANY, so it is
// served over whichever interface has an IP.  Gated on CONFIG_CDC2NET_ETH_W5500.
//
// Probed at boot via esp_eth_driver_install (reads the W5500 chip version): an
// absent / unresponsive W5500 is logged and skipped — WiFi is unaffected.

#ifndef CDC2NET_NET_ETH_H
#define CDC2NET_NET_ETH_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the W5500.  Call AFTER esp_netif_init() + the default event loop
// exist (net_init() does that).  Returns ESP_OK on a detected+started W5500,
// or an error (logged) when no chip is present — the caller ignores the error
// and continues with WiFi.
esp_err_t net_eth_init(void);

// True once the ETH link is up AND an IPv4 has been bound (DHCP/static).
bool net_eth_is_up(void);

// True if a W5500 was detected and its driver is installed (link may be down).
// Used by the WebUI to decide whether to show the Ethernet tile.
bool net_eth_present(void);

// Live ETH IPv4 as "x.x.x.x" (or "0.0.0.0").  Buffer internal.
const char *net_eth_ip_str(void);

// Live ETH default gateway as "x.x.x.x" (or "0.0.0.0" when down).  Buffer internal.
const char *net_eth_gw_str(void);

// The underlying esp_netif handle of the W5500 link, or NULL if no W5500 was
// detected.  Exposed so a higher layer can use it as a routing backbone (e.g.
// the on-device OpenThread Border Router under CONFIG_CDC2NET_MISSION_TBR).
esp_netif_t *net_eth_netif(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_NET_ETH_H
