// SPDX-License-Identifier: GPL-2.0-or-later
//
// CDC2NET — ser2net-style USB-Host-CDC → network bridge on ESP32-S3.
//
// M2 — transparent raw-TCP pipe:
//   source_usb (the CUL) → bridge (fanout) → sink_tcp (raw TCP, multi-client)
// RX from the CUL is fanned to all TCP clients; bytes from any client are
// forwarded to the CUL (serialized by the bridge TX-lock).  WiFi onboarding
// is Improv-Serial on UART0 (from M1).  M3+ adds captive/mDNS, WebUI, OTA.
//
// USB host comes up FIRST (before the WiFi PHY), preserving the #15079
// ordering — proven safe with CONFIG_ESP_PHY_ENABLE_USB=y.

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "version.h"
#include "bridge.h"
#include "source_usb.h"
#include "sink_tcp.h"
#include "net.h"
#include "improv_glue.h"
#include "mdns_glue.h"
#include "log_buffer.h"
#include "webui.h"
#include "config.h"

static const char *TAG = "cdc2net";

static source_t *s_usb;

void app_main(void)
{
    // Log-tee FIRST so the banner + all ESP_LOG lines land in the ring
    // buffer that /api/log serves.
    log_buffer_init();

    printf("\n");
    printf("=================================================\n");
    printf("  CDC2NET   v%s   (M4: pipe + mDNS + WebUI/OTA)\n", FW_VERSION_STRING);
    printf("  built     %s\n", FW_BUILD_DATE);
    printf("  target    ESP32-S3   USB-Host-CDC -> raw-TCP\n");
    printf("=================================================\n");
    ESP_LOGW(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());

    // Bridge + USB source first (USB host before WiFi PHY — #15079 ordering).
    bridge_init();
    s_usb = source_usb_init();
    bridge_attach_source(s_usb);     // wires rx_sink before the CUL opens

    vTaskDelay(pdMS_TO_TICKS(3000)); // let the CUL enumerate before WiFi

    ESP_LOGW(TAG, ">>> bringing up WiFi (PHY init) — USB must survive <<<");
    net_init();
    improv_init();
    mdns_glue_init();    // publishes cdc2net-XXXX.local once WiFi has an IP

    // Raw-TCP listener sink (port from config, default :2329).  Binds once
    // WiFi has an IP (or the SoftAP is up).
    cdc2net_cfg_t cfg;
    config_load(&cfg);
    sink_t *tcp = sink_tcp_init(cfg.tcp_port);
    bridge_attach_sink(tcp, "rawtcp");
    sink_start(tcp);

    // WebUI status/OTA server on :80 (also serves the captive portal).
    webui_init(0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        source_usb_stats_t us;  source_usb_get_stats(&us);
        bridge_stats_t     bs;  bridge_get_stats(&bs);
        sink_tcp_stats_t   ts;  sink_tcp_get_stats(&ts);
        ESP_LOGI(TAG, "STATUS: src=%s | bridge rx=%u tx=%u | tcp :%u cli=%d "
                      "rx=%u tx=%u | net=%s ip=%s | heap=%u",
                 source_describe(s_usb),
                 (unsigned)bs.rx_bytes_total, (unsigned)bs.tx_bytes_total,
                 ts.port, ts.active_clients,
                 (unsigned)ts.rx_bytes_from_clients,
                 (unsigned)ts.tx_bytes_to_clients,
                 net_is_connected() ? "UP" : (net_is_ap_mode() ? "AP" : "down"),
                 net_ip_str(), (unsigned)esp_get_free_heap_size());
    }
}
