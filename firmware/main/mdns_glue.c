// SPDX-License-Identifier: GPL-2.0-or-later
//
// mdns_glue.c — mDNS responder: hostname + short alias + the WebUI service.
// Lifts sta_ipv4() + the short-alias delegate-hostname logic from RFNETHM.
// Advertises only `_http._tcp` (the module-agnostic WebUI on :80); the raw
// stream port is deliberately NOT advertised — what's on the OTG port is
// module-dependent, so a protocol-named service would assert an identity the
// bridge can't know (see mdns_glue.h).

#include "mdns_glue.h"
#include "net.h"
#include "version.h"

#include "mdns.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "mdns";

#define SHORT_ALIAS_HOST "cdc2net"

static bool     s_initialized;
static bool     s_alias_added;
static uint32_t s_alias_ip;

static uint32_t sta_ipv4(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return 0;
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK) return 0;
    return info.ip.addr;
}

// Register/refresh the short alias `cdc2net.local` (in addition to the
// unique `cdc2net-XXXX.local`).  No conflict probing — with several units
// on one net both answer; the unique form stays the stable path.
static void update_short_alias(void)
{
    uint32_t ip = sta_ipv4();
    if (!ip) return;
    if (s_alias_added && ip == s_alias_ip) return;

    mdns_ip_addr_t addr = {0};
    addr.addr.type            = ESP_IPADDR_TYPE_V4;
    addr.addr.u_addr.ip4.addr = ip;
    addr.next                 = NULL;

    if (!s_alias_added) {
        esp_err_t err = mdns_delegate_hostname_add(SHORT_ALIAS_HOST, &addr);
        if (err == ESP_OK) {
            s_alias_added = true;
            s_alias_ip    = ip;
            ESP_LOGI(TAG, "alias %s.local -> " IPSTR " published",
                     SHORT_ALIAS_HOST, IP2STR(&addr.addr.u_addr.ip4));
        } else {
            ESP_LOGW(TAG, "delegate_hostname_add(%s) failed: %s",
                     SHORT_ALIAS_HOST, esp_err_to_name(err));
        }
    } else {
        if (mdns_delegate_hostname_set_address(SHORT_ALIAS_HOST, &addr) == ESP_OK)
            s_alias_ip = ip;
    }
}

static void mdns_task(void *arg)
{
    (void)arg;

    // Wait until STA has an IP, else mDNS binds to an interface with no
    // address and replies reach no one.
    while (!net_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    s_initialized = true;

    if (mdns_hostname_set(net_hostname()) != ESP_OK) {
        ESP_LOGW(TAG, "mdns_hostname_set('%s') failed", net_hostname());
    }
    mdns_instance_name_set("CDC2NET USB-CDC Bridge");

    // Advertise the WebUI (_http._tcp on :80) — this is module-agnostic, so
    // unlike the raw stream port it asserts no protocol identity the bridge
    // can't know.  A single TXT key carries the firmware version so a browser
    // (or `avahi-browse`) can tell units apart without opening the page.
    mdns_txt_item_t http_txt[] = {
        { "fw", FW_VERSION_STRING },
        { "dev", "cdc2net" },
    };
    esp_err_t serr = mdns_service_add("CDC2NET WebUI", "_http", "_tcp", 80,
                                      http_txt, sizeof(http_txt) / sizeof(http_txt[0]));
    if (serr != ESP_OK)
        ESP_LOGW(TAG, "mdns_service_add(_http._tcp) failed: %s", esp_err_to_name(serr));

    ESP_LOGI(TAG, "mDNS up: %s.local (+ %s.local alias) — _http._tcp:80 advertised, stream port NOT",
             net_hostname(), SHORT_ALIAS_HOST);
    update_short_alias();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        update_short_alias();
    }
}

esp_err_t mdns_glue_init(void)
{
    if (s_initialized) return ESP_OK;
    BaseType_t r = xTaskCreate(mdns_task, "mdns", 4096, NULL, 4, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
