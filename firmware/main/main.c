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
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "version.h"
#include "bridge.h"
#include "source.h"
// Source selection (Kconfig choice CDC2NET_SOURCE_*).  v0.1 ships USB only; the
// UART (EUL/TUL serial downlink) and Thread/RCP sources slot in here as extra
// #elif branches when their missions land (see docs/merge-targets.md).
// Both USB options implement the same source_usb.h contract (source_usb_init);
// the Kconfig choice decides which .c is compiled — esp-usb or CherryUSB.
#if defined(CONFIG_CDC2NET_SOURCE_USB) || defined(CONFIG_CDC2NET_SOURCE_USB_CHERRY)
#include "source_usb.h"
#elif defined(CONFIG_CDC2NET_SOURCE_UART)
#include "source_uart.h"
#else
#error "no CDC2NET source selected — set a CONFIG_CDC2NET_SOURCE_* option (Kconfig.projbuild)"
#endif
#include "sink_tcp.h"
#include "net.h"
#include "improv_glue.h"
#include "mdns_glue.h"
#include "log_buffer.h"
#include "webui.h"
#include "config.h"
#include "health.h"
#include "app_wdt.h"
#ifdef CDC2NET_HUBPROBE
#include "hubprobe.h"     // THROWAWAY USB-host channel-budget probe (env:hubprobe)
#endif
#if defined(CONFIG_CDC2NET_EEPROM_M24C32)
#include "eeprom_m24c32.h"   // M24C32 config-EEPROM boot self-test (C6 EUL carrier)
#endif
#if defined(CONFIG_CDC2NET_RADIO_FLASH)
#include "radio_flash.h"     // companion-radio (H2) flashed from the embedded image
#endif
#if defined(CONFIG_CDC2NET_MISSION_TBR)
#include "mission_tbr.h"     // experimental on-device OpenThread BR on W5500 backbone
#endif

static const char *TAG = "cdc2net";

static source_t *s_src;

// Task-WDT state for app_main() (see app_wdt.h).  s_main_wdt_task is set once
// the main task is on the watchdog; NULL means "never got guarded".
static TaskHandle_t  s_main_wdt_task;
static bool          s_wdt_on;       // main currently subscribed?
static volatile bool s_wdt_paused;   // set by another task around a long op

// Called from another task (the OTA upload handler).  Unsubscribing works from
// anywhere; re-subscribing deliberately does NOT happen here, because a task
// that has just been added counts as not-yet-fed and only the task itself can
// feed.  Resume therefore just clears the flag and lets the main loop re-arm
// itself, where it can subscribe and feed in one go.
void app_wdt_pause_main(void)
{
    if (s_main_wdt_task && s_wdt_on) {
        esp_task_wdt_delete(s_main_wdt_task);
        s_wdt_on = false;
    }
    s_wdt_paused = true;
}

void app_wdt_resume_main(void)
{
    s_wdt_paused = false;
}

// Feed from inside the main loop; re-arms after a pause.
static void app_wdt_feed_main(void)
{
    if (s_wdt_paused || !s_main_wdt_task) {
        return;
    }
    if (!s_wdt_on) {
        if (esp_task_wdt_add(NULL) != ESP_OK) {
            return;
        }
        s_wdt_on = true;
    }
    esp_task_wdt_reset();
}

void app_main(void)
{
#ifdef CDC2NET_HUBPROBE
    // Throwaway probe: bring up USB host + hub support ONLY, no WiFi/bridge/UI.
    hubprobe_run();
    return;
#endif
    // Log-tee FIRST so the banner + all ESP_LOG lines land in the ring
    // buffer that /api/log serves.
    log_buffer_init();

    printf("\n");
    printf("=================================================\n");
    printf("  CDC2NET   v%s   (M4: pipe + mDNS + WebUI/OTA)\n", FW_VERSION_STRING);
    printf("  built     %s\n", FW_BUILD_DATE);
#if defined(CONFIG_CDC2NET_SOURCE_USB)
    printf("  source    USB-Host-CDC (CUL/TUL/EUL + FTDI/CH34x/CP210x)\n");
#elif defined(CONFIG_CDC2NET_SOURCE_USB_CHERRY)
    printf("  source    USB-Host-CDC via CherryUSB (CUL/TUL/EUL + FTDI/CH34x/CP210x)\n");
#elif defined(CONFIG_CDC2NET_SOURCE_UART)
    printf("  source    onboard radio over UART (EUL/TUL serial downlink)\n");
#endif
    printf("  target    %s   -> raw-TCP\n", CONFIG_IDF_TARGET);
    printf("=================================================\n");
    ESP_LOGW(TAG, "boot: reset_reason=%d", (int)esp_reset_reason());

    // NVS must be up BEFORE any source can open a device.  The USB source
    // resolves its per-device line coding through serialcfg_lookup() ->
    // nvs_open() as soon as a stick enumerates (~1 s), which is well before
    // net_init() runs (it sits behind the 3 s USB settle below) and used to be
    // the first thing to call nvs_flash_init().  nvs_open() on uninitialised
    // NVS just fails, and the lookup silently degrades to the compiled-in
    // default — so a stick configured to e.g. 19200 8E1 came back up at
    // 115200 8N1 after every reboot, and only a live WebUI POST fixed it until
    // the next one.  nvs_flash_init() is idempotent, so net_init()'s own call
    // (which also owns the erase-and-retry recovery) still behaves as before.
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (nerr != ESP_OK) {
        ESP_ERROR_CHECK(nerr);
    }

    // Bridge + source first (USB host before WiFi PHY — #15079 ordering).
    bridge_init();

#ifdef CONFIG_CDC2NET_RADIO_FLASH
    // Companion radio (S3+H2 gateway board) gets its firmware from THIS image,
    // before anything else claims the inter-chip UART — the flasher owns the
    // port and the reset/boot straps for the duration and releases both again.
    // No-ops when the radio already runs the embedded version; never fatal.
    radio_flash_sync();
#endif
#if defined(CONFIG_CDC2NET_SOURCE_USB) || defined(CONFIG_CDC2NET_SOURCE_USB_CHERRY)
    s_src = source_usb_init();
#elif defined(CONFIG_CDC2NET_SOURCE_UART)
    s_src = source_uart_init();
#else
    // Symmetric with the include block above: a future mission that adds a
    // source to the Kconfig choice + include block but forgets its construct
    // #elif here fails loudly at compile time instead of leaving s_src NULL.
#error "no CDC2NET source constructed — add the matching CONFIG_CDC2NET_SOURCE_* branch"
#endif
    bridge_attach_source(s_src);     // wires rx_sink before the source opens

    vTaskDelay(pdMS_TO_TICKS(3000)); // let the CUL enumerate before WiFi

    ESP_LOGW(TAG, ">>> bringing up WiFi (PHY init) — USB must survive <<<");
    net_init();
    health_boot_init();  // NVS is up now (net_init did nvs_flash_init) — bump
                         // boot/crash counters, attribute the last reset
    improv_init();
    mdns_glue_init();    // publishes cdc2net-XXXX.local once WiFi has an IP

    // Raw-TCP listener sink (port from config, default :2329).  Binds once
    // WiFi has an IP (or the SoftAP is up).
    cdc2net_cfg_t cfg;
    config_load(&cfg);
    sink_t *tcp = sink_tcp_init(cfg.tcp_port);
    bridge_attach_sink(tcp, "rawtcp");
    sink_start(tcp);

#if defined(CONFIG_CDC2NET_EEPROM_M24C32)
    // M24C32 config-EEPROM presence + non-destructive RMW self-test (C6 EUL
    // carrier).  Independent of net/NVS; runs before webui so /api/status has
    // the result on its first poll.  Absent chip → state 0, never asserts.
    eeprom_init_and_test();
#endif

    // WebUI status/OTA server on :80 (also serves the captive portal).
    webui_init(0);

#if defined(CONFIG_CDC2NET_MISSION_TBR)
    // Experimental: bring up a full on-device OpenThread Border Router on the
    // W5500 backbone (C6 native 802.15.4 radio), alongside the bridge.
    mission_tbr_start();
#endif

    // Subscribe this (main) task to the Task-WDT so a wedged firmware path is
    // caught and panic-rebooted (CONFIG_ESP_TASK_WDT_TIMEOUT_S=5, PANIC=y).
    // The STATUS cadence stays 10 s, but we feed the WDT every 1 s inside the
    // sleep so a healthy loop never trips the 5 s timeout.  (RFNETHM subscribes
    // its source supervisor; the family pattern is to put a real app task on
    // the WDT rather than rely on idle-task monitoring alone.)
    esp_err_t wr = esp_task_wdt_add(NULL);
    if (wr != ESP_OK && wr != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "esp_task_wdt_add failed (%s) — main loop not WDT-guarded",
                 esp_err_to_name(wr));
    } else {
        // Feed once right away: with CONFIG_ESP_TASK_WDT_INIT the watchdog timer
        // has been free-running since boot, and a freshly added task starts out
        // marked as not-yet-fed.  Whether that trips depends only on where in
        // the timer's period this subscribe lands — the loop below does not
        // feed until after its first 1 s sleep.  Observed twice as a TASK_WDT
        // panic 240/270 ms after subscribing; across the captured boots this
        // point always sits at ~8.2-8.5 s, and the two that tripped were not
        // the slow ones (their WiFi association was the fastest measured), so
        // boot duration does not predict it and phase alone explains it.
        esp_task_wdt_reset();
    }
    health_set_wdt_subscribed(wr == ESP_OK || wr == ESP_ERR_INVALID_ARG);
    if (wr == ESP_OK || wr == ESP_ERR_INVALID_ARG) {
        s_main_wdt_task = xTaskGetCurrentTaskHandle();
        s_wdt_on        = true;
    }

    while (1) {
        for (int i = 0; i < 10; i++) {       // 10 x 1 s = 10 s STATUS cadence
            vTaskDelay(pdMS_TO_TICKS(1000));
            app_wdt_feed_main();             // feed well under the 5 s timeout
        }
        bridge_stats_t     bs;  bridge_get_stats(&bs);
        sink_tcp_stats_t   ts;  sink_tcp_get_stats(&ts);
        ESP_LOGI(TAG, "STATUS: src=%s | bridge rx=%u tx=%u | tcp :%u cli=%d "
                      "rx=%u tx=%u | net=%s ip=%s | heap=%u",
                 source_describe(s_src),
                 (unsigned)bs.rx_bytes_total, (unsigned)bs.tx_bytes_total,
                 ts.port, ts.active_clients,
                 (unsigned)ts.rx_bytes_from_clients,
                 (unsigned)ts.tx_bytes_to_clients,
                 net_is_connected() ? "UP" : (net_is_ap_mode() ? "AP" : "down"),
                 net_ip_str(), (unsigned)esp_get_free_heap_size());
    }
}
