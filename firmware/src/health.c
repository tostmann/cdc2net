// SPDX-License-Identifier: GPL-2.0-or-later

#include "health.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "health";

#define NS       "cdc2net"        // shared with config.c + net.c creds
#define K_BOOT   "boot_cnt"
#define K_CRASH  "crash_cnt"
#define K_WDTRB  "wdt_reboots"

static uint32_t s_boot;
static uint32_t s_crash;
static uint32_t s_wdt_reboots;
static bool     s_wdt_subscribed;

static uint32_t nvs_u32_or_0(nvs_handle_t h, const char *k)
{
    uint32_t v = 0;
    nvs_get_u32(h, k, &v);   // leaves v=0 if the key is absent
    return v;
}

void health_boot_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
                  r == ESP_RST_INT_WDT || r == ESP_RST_WDT ||
                  r == ESP_RST_BROWNOUT);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed — health counters not persisted this boot");
        return;
    }
    s_boot        = nvs_u32_or_0(h, K_BOOT) + 1;
    s_crash       = nvs_u32_or_0(h, K_CRASH) + (crash ? 1 : 0);
    s_wdt_reboots = nvs_u32_or_0(h, K_WDTRB);

    nvs_set_u32(h, K_BOOT, s_boot);
    if (crash) nvs_set_u32(h, K_CRASH, s_crash);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "boot #%u (reset=%d%s) | crashes=%u conn-wdt-reboots=%u",
             (unsigned)s_boot, (int)r, crash ? " CRASH" : "",
             (unsigned)s_crash, (unsigned)s_wdt_reboots);
}

void health_note_connectivity_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t v = nvs_u32_or_0(h, K_WDTRB) + 1;
    nvs_set_u32(h, K_WDTRB, v);
    nvs_commit(h);
    nvs_close(h);
    s_wdt_reboots = v;
}

void health_set_wdt_subscribed(bool on) { s_wdt_subscribed = on; }

uint32_t health_boot_count(void)     { return s_boot; }
uint32_t health_crash_count(void)    { return s_crash; }
uint32_t health_wdt_reboots(void)    { return s_wdt_reboots; }
bool     health_wdt_subscribed(void) { return s_wdt_subscribed; }
