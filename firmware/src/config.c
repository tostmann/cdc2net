// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config";

#define CFG_NS  "cdc2net"   // shared with net.c (wifi_ssid/wifi_pass)

void config_load(cdc2net_cfg_t *out)
{
    if (!out) return;
    // Defaults.
    memset(out, 0, sizeof(*out));
    out->tcp_port      = CONFIG_TCP_PORT_DEFAULT;
    out->static_ip     = false;
    out->wdt_enable    = false;
    out->wdt_timeout_s = CONFIG_WDT_TIMEOUT_DEFAULT;

    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint16_t u16; uint8_t u8; uint32_t u32; size_t sz;
    if (nvs_get_u16(h, "tcp_port", &u16) == ESP_OK && u16) out->tcp_port = u16;
    if (nvs_get_u8 (h, "static_ip", &u8) == ESP_OK)        out->static_ip = (u8 != 0);
    sz = sizeof(out->ip);   nvs_get_str(h, "ip",   out->ip,   &sz);
    sz = sizeof(out->mask); nvs_get_str(h, "mask", out->mask, &sz);
    sz = sizeof(out->gw);   nvs_get_str(h, "gw",   out->gw,   &sz);
    sz = sizeof(out->dns);  nvs_get_str(h, "dns",  out->dns,  &sz);
    if (nvs_get_u8 (h, "wdt_en", &u8)  == ESP_OK)          out->wdt_enable = (u8 != 0);
    if (nvs_get_u32(h, "wdt_to", &u32) == ESP_OK && u32)   out->wdt_timeout_s = u32;

    nvs_close(h);
}

esp_err_t config_save(const cdc2net_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_u16(h, "tcp_port", cfg->tcp_port);
    if (err == ESP_OK) err = nvs_set_u8 (h, "static_ip", cfg->static_ip ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "ip",   cfg->ip);
    if (err == ESP_OK) err = nvs_set_str(h, "mask", cfg->mask);
    if (err == ESP_OK) err = nvs_set_str(h, "gw",   cfg->gw);
    if (err == ESP_OK) err = nvs_set_str(h, "dns",  cfg->dns);
    if (err == ESP_OK) err = nvs_set_u8 (h, "wdt_en", cfg->wdt_enable ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, "wdt_to", cfg->wdt_timeout_s);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "saved: tcp_port=%u static_ip=%d ip=%s wdt=%d/%us",
                 cfg->tcp_port, cfg->static_ip, cfg->ip,
                 cfg->wdt_enable, (unsigned)cfg->wdt_timeout_s);
    else
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}
