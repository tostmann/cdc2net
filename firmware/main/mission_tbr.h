// SPDX-License-Identifier: GPL-2.0-or-later
//
// mission_tbr.h — experimental on-device Thread Border Router for the C6
// generation (gated on CONFIG_CDC2NET_MISSION_TBR, default n).
//
// Brings up a full OpenThread Border Router on the ESP32-C6 native 802.15.4
// radio, using the W5500 ethernet netif (net_eth.c) as the IP backbone, running
// ALONGSIDE the EUL/TUL UART-source bridge.  Lets a single C6 stick act as both
// a serial bridge AND a Thread border router (no RCP / second radio).
//
// Validated on the bench: combined bridge+OTBR fits one 1.9375 MB OTA slot on
// the 4 MB C6 module; a commercial Matter-over-Thread device commissions onto
// it and routes via the W5500 backbone.  See docs / the merge-targets matrix.

#ifndef CDC2NET_MISSION_TBR_H
#define CDC2NET_MISSION_TBR_H

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the on-device OpenThread Border Router on the W5500 backbone.
// Call AFTER net_init() (the W5500 netif must already exist) and the rest of
// the CDC2NET bringup.  No-op if the W5500 backbone netif is absent.
void mission_tbr_start(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_MISSION_TBR_H
