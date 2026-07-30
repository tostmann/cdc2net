// SPDX-License-Identifier: GPL-2.0-or-later
//
// Improv-Serial-WiFi-Provisioning für CDC2NET.  Der Improv-Byte-Strom
// kommt über die *Konsole* herein — und die ist je nach Chip verschieden
// verdrahtet:
//   - S3 (YD-ESP32-S3, CH343P USB-UART-Bridge):  UART0
//   - C3/C6 (EUL/TUL, native USB):               USB-Serial-JTAG
// Welcher Transport gilt, entscheidet die Konsolen-Kconfig
// (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) zur Compile-Zeit; ein einziges
// improv_task treibt beide über die transport_*-Helfer.  (Vor v0.1.70 war
// das hart auf UART0 verdrahtet → auf den USB-JTAG-Konsolen-Targets C3/C6
// erreichten die Host-Bytes den Reader nie = Improv tot.)  Lib-Quelle:
// https://github.com/tostmann/improv-wifi-busware (Pin in
// idf_component.yml).
//
// Architektur (analog CULFW32, vereinfacht):
//   - Konsolen-Transport-Driver wird hier installiert (RX-Buffer 512 B).
//   - improv_task liest Bytes vom Transport, füttert die Lib via feedBytes().
//   - write_fn schreibt Antwort-Bytes über denselben Transport zurück.
//   - Tick wird im Task-Loop einmal pro 100 ms gerufen.
//   - onConnected → net_persist_creds() (Atomar, kein Reboot — die
//     Lib's tryConnect hat den STA bereits hochgezogen, persisting nur
//     für den nächsten Boot).
//   - net_set_external_control(true) ist während des Windows aktiv,
//     damit unser eigener STA_DISCONNECTED-Handler nicht in die
//     re-provision-Sequenz reinpfuscht.
//
// Stick-Console: das v0.9-Banner und alle ESP_LOG_* gehen weiterhin
// via printf auf UART0 raus.  Die Lib parst nur eingehende Bytes mit
// dem `IMPROV`-Magic; unsere Logs enthalten das Magic nicht, also keine
// Konflikte.

#include "improv_glue.h"
#include "net.h"

#include "improv_wifi/improv_wifi.h"
#include "improv_wifi/idf_backend.h"
#include "improv_wifi/types.h"

#include "version.h"

// Console transport selection (see file header).  esp_driver_usb_serial_jtag
// is already a public REQUIRES of the legacy `driver` component, so no extra
// CMake REQUIRES is needed for either header.
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#define IMPROV_TRANSPORT_USJ 1
#include "driver/usb_serial_jtag.h"
#else
#define IMPROV_TRANSPORT_USJ 0
#include "driver/uart.h"
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdint>
#include <cstring>

static const char *TAG = "improv";

namespace ipw = improv_wifi_busware;

namespace {

#if !IMPROV_TRANSPORT_USJ
constexpr uart_port_t UART_NUM         = UART_NUM_0;
constexpr int         UART_RX_BUF      = 512;
constexpr int         UART_TX_BUF      = 0;     // 0 = blocking writes (sync)
#endif
// Idle-basiertes Timeout: das Window bleibt offen solange host→stick
// Bytes innerhalb der letzten IDLE_MS-Spanne gekommen sind.  Erst
// `IDLE_MS` ohne jeglichen UART-Byte → Window geht zu.  Damit kann der
// User entspannt reconfiguraten (ESP Web Tools öffnen, scannen, tippen,
// senden) ohne dass ihm mitten drin der Boden weggezogen wird.
//
// Initial-Idle (= "ab Boot ohne Aktivität"):
//   - Keine Creds:    120 s — Erstprovisioning, langsamer User
//   - Creds da:        30 s — schnelle Reconfig, sonst zu
//
// Lib-internes windowMs ist auf ≫ Idle gesetzt; wir steuern das Window
// effektiv selbst über die Idle-Bookkeeping unten.
constexpr uint32_t    IDLE_MS_FRESH      = 120 * 1000;
constexpr uint32_t    IDLE_MS_HASCREDS   =  30 * 1000;
constexpr uint32_t    LIB_WINDOW_MS_CAP  =  60 * 60 * 1000;  // 1 h hard-cap

ipw::EspIdfWiFiBackend *s_backend          = nullptr;
ipw::ImprovWiFi        *s_inst             = nullptr;
bool                    s_inited           = false;
int64_t                 s_last_activity_us = 0;
uint32_t                s_idle_ms          = IDLE_MS_FRESH;
bool                    s_armed            = false;

// ── Console transport abstraction (UART0 ↔ USB-Serial-JTAG) ────────────
// Installs the RX/TX driver for whichever console the chip uses, and
// wraps read/write/quiesce so improv_task / write_fn stay transport-blind.

esp_err_t transport_install(void)
{
#if IMPROV_TRANSPORT_USJ
    // USB-Serial-JTAG: the console runs in no-driver (ROM/VFS) mode by
    // default; installing the driver attaches an ISR that drains the RX
    // FIFO into a ringbuffer so usb_serial_jtag_read_bytes() works.  printf
    // keeps writing via the ROM path — same coexistence as UART0+console.
    usb_serial_jtag_driver_config_t c = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    c.rx_buffer_size = 512;
    // >= the improv lib's structural max single write (9 + 255 + 1 = 265 B).
    // usb_serial_jtag_write_bytes is all-or-nothing into a byte-ringbuffer
    // whose max item == its size, so a 256-B TX buffer could silently drop a
    // maximal response frame.  Real frames stay small, but the critical
    // improv-response path is worth the ~256 B of RAM to make it impossible.
    c.tx_buffer_size = 512;
    esp_err_t err = usb_serial_jtag_driver_install(&c);
    if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;   // already installed
    return err;
#else
    // UART0-Driver installieren — printf nutzt UART0 weiter via VFS,
    // aber wir lesen RX direkt vom Driver.
    uart_config_t cfg = {};
    cfg.baud_rate  = 115200;
    cfg.data_bits  = UART_DATA_8_BITS;
    cfg.parity     = UART_PARITY_DISABLE;
    cfg.stop_bits  = UART_STOP_BITS_1;
    cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    // ESP-IDF-Konvention: uart_driver_install ZUERST, dann uart_param_config
    // — manche IDF-Versionen werden bei umgekehrter Reihenfolge strenger.
    // ESP_ERR_INVALID_STATE bei install tolerieren (ROM-Default-Driver).
    esp_err_t err = uart_driver_install(UART_NUM, UART_RX_BUF, UART_TX_BUF, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = uart_param_config(UART_NUM, &cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
#endif
}

inline int transport_read(uint8_t *buf, size_t len, TickType_t ticks)
{
#if IMPROV_TRANSPORT_USJ
    return usb_serial_jtag_read_bytes(buf, len, ticks);
#else
    return uart_read_bytes(UART_NUM, buf, len, ticks);
#endif
}

inline void transport_write(const uint8_t *data, size_t len)
{
#if IMPROV_TRANSPORT_USJ
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
#else
    uart_write_bytes(UART_NUM, reinterpret_cast<const char *>(data), len);
#endif
}

inline void transport_rx_quiesce(void)
{
#if IMPROV_TRANSPORT_USJ
    // No RX FIFO to mask: the task just stops reading.  The 512-B driver
    // ringbuffer only refills if the host keeps sending, which it won't
    // after the window closes — benign.
#else
    // RX-Pfad abdrehen, sonst sammelt der 512-B-Buffer Müll vom Terminal
    // bis er voll ist und verwirft dann weitere Bytes.
    uart_disable_rx_intr(UART_NUM);
    uart_flush_input(UART_NUM);
#endif
}

void write_fn(const uint8_t *data, size_t len, void * /*user*/)
{
    if (!data || len == 0) return;
    transport_write(data, len);
}

void on_error_cb(ipw::Error e, void * /*user*/)
{
    ESP_LOGW(TAG, "improv error 0x%02x", static_cast<unsigned>(e));
}

void on_connected_cb(const char *ssid, const char *psk, void * /*user*/)
{
    ESP_LOGI(TAG, "provisioned to '%s' — persisting in cdc2net NVS",
             ssid ? ssid : "?");
    esp_err_t err = net_persist_creds(ssid ? ssid : "", psk ? psk : "");
    if (err != ESP_OK) {
        // STA läuft live, aber NVS-Schreiben gescheitert → nächster Reboot
        // landet im Captive-AP.  Hier können wir nichts mehr machen ausser
        // den User per Log warnen; Improv selbst hat schon "Success" an den
        // BLE-Client zurückgemeldet.
        ESP_LOGE(TAG, "net_persist_creds failed (%s) — creds NOT persisted, "
                      "next reboot falls back to Captive-AP",
                 esp_err_to_name(err));
    }
    // Kein Reboot — Lib hat den STA bereits hochgezogen, der nächste
    // Boot nimmt die persistierten Creds und beschneidet das Improv-
    // Window auf WINDOW_MS_HASCREDS (30 s) — kurz, aber nicht aus,
    // damit Re-Provisioning via ESP Web Tools jederzeit möglich bleibt
    // (WLAN-Wechsel ohne Captive-AP-Ritual).
}

void improv_task(void * /*arg*/)
{
    uint8_t buf[64];
    while (true) {
        const int64_t  now_us = esp_timer_get_time();
        const uint32_t now_ms = static_cast<uint32_t>(now_us / 1000);
        s_inst->tick(now_ms);

        // Eigene Idle-Logik: Window läuft solange Aktivität innerhalb der
        // letzten s_idle_ms — und solange die Lib selbst nicht den 1 h-
        // Hard-Cap erreicht (Sicherheits-Cap, z.B. gegen Dauerprasseln).
        const int64_t idle_us = now_us - s_last_activity_us;
        const bool    expired = idle_us > static_cast<int64_t>(s_idle_ms) * 1000;
        if (!s_inst->isArmed() || expired) {
            ESP_LOGI(TAG, "window closed (idle %us, lib_armed=%d)",
                     static_cast<unsigned>(idle_us / 1000000),
                     static_cast<int>(s_inst->isArmed()));
            s_armed = false;
            net_set_external_control(false);
            // Driver eingebaut lassen — printf/ESP_LOG nutzen die Konsole
            // weiter und brauchen den installed driver.  Nur den RX-Pfad
            // ruhigstellen (siehe transport_rx_quiesce).
            transport_rx_quiesce();
            break;
        }

        const int got = transport_read(buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (got > 0) {
            // Jedes empfangene Byte zählt als Aktivität — auch wenn die
            // Lib das nicht als Improv-Magic erkennt (z.B. der User tippt
            // versehentlich was im Terminal).  Konservativ: das Idle-
            // Fenster lieber zu lang als zu kurz schließen.
            s_last_activity_us = now_us;
            s_inst->feedBytes(buf, static_cast<size_t>(got));
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace

extern "C" esp_err_t improv_init(void)
{
    if (s_inited) return ESP_OK;

    // Konsolen-Transport-Driver installieren (UART0 oder USB-Serial-JTAG,
    // je nach Chip/Konsole — siehe transport_install).
    esp_err_t err = transport_install();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transport_install failed: %s", esp_err_to_name(err));
        return err;
    }

    // Lib-Backend + Instanz statisch — Lebensdauer = Programm.
    static ipw::EspIdfWiFiBackend s_be;
    s_backend = &s_be;

    s_idle_ms = net_has_creds() ? IDLE_MS_HASCREDS : IDLE_MS_FRESH;
    s_last_activity_us = esp_timer_get_time();
    s_armed = true;

    ipw::Config c;
    c.backend                  = s_backend;
    c.write                    = &write_fn;
    c.userCtx                  = nullptr;
    c.windowMs                 = LIB_WINDOW_MS_CAP;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    c.device.chipFamily        = ipw::ChipFamily::Esp32S3;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    c.device.chipFamily        = ipw::ChipFamily::Esp32C3;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    c.device.chipFamily        = ipw::ChipFamily::Esp32C6;
#else
    c.device.chipFamily        = ipw::ChipFamily::Esp32;
#endif
    c.device.firmwareName      = "CDC2NET";
    c.device.firmwareVersion   = FW_VERSION_STRING;
    c.device.deviceName        = "CDC2NET";
    c.device.deviceUrl         = nullptr;   // Lib füllt http://<ip>/
    c.onError                  = &on_error_cb;
    c.onConnected              = &on_connected_cb;

    static ipw::ImprovWiFi s_improv_inst{c};
    s_inst = &s_improv_inst;

    // Solange das Window armed ist, nehmen wir net.c den Auto-Reconnect
    // weg — sonst kollidieren wir mit Lib's tryConnect/disconnect-
    // Sequenz.
    net_set_external_control(true);

    BaseType_t r = xTaskCreate(&improv_task, "improv", 4096, nullptr, 4, nullptr);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "improv-serial armed: idle-timeout=%us (creds %spresent), "
                  "extends on UART activity",
             static_cast<unsigned>(s_idle_ms / 1000),
             net_has_creds() ? "" : "not ");
    return ESP_OK;
}

extern "C" bool improv_is_armed(void)
{
    return s_armed && s_inst && s_inst->isArmed();
}

extern "C" uint32_t improv_window_remaining_ms(void)
{
    // Verbleibende Idle-Zeit, NICHT das absolute Lib-Window — passt zum
    // refactor in v0.14.55: Window läuft nach IDLE_MS Stille zu, nicht
    // nach absolutem Timer ab Boot.
    if (!s_armed) return 0;
    const int64_t idle_us = esp_timer_get_time() - s_last_activity_us;
    const int64_t left_us = static_cast<int64_t>(s_idle_ms) * 1000 - idle_us;
    return left_us > 0 ? static_cast<uint32_t>(left_us / 1000) : 0;
}
