// SPDX-License-Identifier: GPL-2.0-or-later
//
// config.h — persisted device settings (NVS namespace "cdc2net", alongside
// the wifi_ssid/wifi_pass keys owned by net.c).  Loaded at boot; changes are
// saved via POST /api/config and applied on reboot.

#ifndef CDC2NET_CONFIG_H
#define CDC2NET_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_TCP_PORT_DEFAULT   2329
#define CONFIG_WDT_TIMEOUT_DEFAULT 300   // seconds

typedef struct {
    uint16_t tcp_port;          // raw-TCP listener port
    bool     static_ip;         // false = DHCP
    char     ip[16];            // dotted IPv4, only used when static_ip
    char     mask[16];
    char     gw[16];
    char     dns[16];
    bool     wdt_enable;        // connectivity watchdog
    uint32_t wdt_timeout_s;     // reboot after this long without an STA IP
} cdc2net_cfg_t;

// Fill *out with defaults, then overlay any values stored in NVS.
void      config_load(cdc2net_cfg_t *out);

// Persist *cfg to NVS.  Returns ESP_OK on success.
esp_err_t config_save(const cdc2net_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_CONFIG_H
