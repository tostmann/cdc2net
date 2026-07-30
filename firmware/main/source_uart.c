// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_uart.c — transparent hardware-UART source for CDC2NET (EUL/TUL).
//
// The EUL/TUL sticks carry their radio/bus module on an internal UART, not on
// a USB-host port.  This source is the UART analogue of source_usb: a fully
// byte-transparent pipe.  RX bytes go straight to the bridge fanout; bridge TX
// bytes go straight to the module.  There is NO on-device framing/protocol —
// the host (FHEM / knxd) speaks the module's native wire protocol end-to-end
// through the pipe.  Per-port baud is handled through the same serialcfg /
// RFC2217 line-coding ops as source_usb.
//
// Two module profiles share this one source (select the second with
// -D UART_MODULE_NCN5130; default = TCM515):
//   EUL  EnOcean TCM515 : 460800 8N1.  GPIO bringup — SET (LOW=operational
//        baud) + active-low RST pulse.  FHEM speaks ESP3 (like a USB300/LAN).
//        Blueprint = EULFW32 cul32-net (C6): UART1 TX=GPIO4 RX=GPIO5 RST=GPIO3
//        SET=GPIO2.
//   TUL  ON-Semi NCN5130 KNX-TP : 38400 8E1 (KNX TP-UART host format = ip4knx
//        KNX_BAUDRATE).  NO GPIO bringup and NO host register init — RX/TX are
//        galvanically isolated (ISO7221), no reset/mode line crosses, and the
//        NCN operates without an ACR0/V20V write (ESP is not bus-powered → no
//        NCN power concern).  FHEM/knxd drive the TP-UART protocol (incl. their
//        own U_Reset) end-to-end.  Pins per ip4knx tul_esp32c3: TX=21 RX=20,
//        RST/SET = -1.
//
// All pins / baud are #define-overridable via -D build flags for other boards.

#include "esp_timer.h"

#include "sink_tcp.h"     // sink_tcp_drop_clients() bei Radio-Reboot
#include "source_uart.h"
#include "bridge.h"
#include "serialcfg.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "src-uart";

// ── Board wiring (EUL/TUL C6 generation; override per carrier via -D) ──────
#ifndef UART_TCM_PORT
#define UART_TCM_PORT   UART_NUM_1
#endif
#ifndef UART_TCM_TX
#define UART_TCM_TX     4
#endif
#ifndef UART_TCM_RX
#define UART_TCM_RX     5
#endif
#ifndef UART_TCM_RST            // active-low reset to the radio module (-1 = none)
#define UART_TCM_RST    3
#endif
#ifndef UART_TCM_SET            // mode/baud select (LOW=operational) (-1 = none)
#define UART_TCM_SET    2
#endif
#ifndef UART_TCM_BAUD           // EnOcean TCM515 operational baud (SET=LOW)
#define UART_TCM_BAUD   460800
#endif

// ── Module profile ─────────────────────────────────────────────────────────
// Only the default line coding, the serialcfg key and the display labels
// differ between modules; the byte-transparent core below is identical.
// Select the NCN5130 (TUL) profile with -D UART_MODULE_NCN5130; default = EUL
// TCM515.  Line coding is still per-port overridable (NVS default + RFC2217).
#ifdef UART_MODULE_NCN5130
  #define UART_MODULE_NAME   "NCN5130"
  #define UART_MODULE_LABEL  "TUL NCN5130 (UART)"
  #define UART_KEY           "ncn5130"   // single onboard device → fixed key
  #define UART_DEF_BAUD      38400        // KNX TP-UART host baud (= ip4knx)
  #define UART_DEF_PARITY    2            // even  ┐
  #define UART_DEF_STOP      0            // 1     ┴─→ 8E1
#elif defined(UART_MODULE_ZB_NCP)
  // ESP Zigbee-Gateway board: the "module" on this UART is the ESP32-H2 running
  // the ZBOSS NCP firmware.  Without this profile the build would fall through
  // to the EnOcean default and the web UI would announce a TCM515 that is not
  // there — the radio-firmware details come from radio_flash.c, see radio_info.
  #define UART_MODULE_NAME   "ZB-NCP"
  #define UART_MODULE_LABEL  "ESP32-H2 Zigbee NCP (UART)"
  #define UART_KEY           "zbncp"      // single onboard device → fixed key
  #define UART_DEF_BAUD      UART_TCM_BAUD // must match CONFIG_NCP_UART_BAUD
  #define UART_DEF_PARITY    0            // none ┐
  #define UART_DEF_STOP      0            // 1    ┴─→ 8N1
#else
  #define UART_MODULE_NAME   "TCM515"
  #define UART_MODULE_LABEL  "EUL TCM515 (UART)"
  #define UART_KEY           "tcm515"     // single onboard device → fixed key
  #define UART_DEF_BAUD      UART_TCM_BAUD // EnOcean operational ┐
  #define UART_DEF_PARITY    0            // none                ┴─→ 8N1
  #define UART_DEF_STOP      0            // 1
#endif
#define UART_DEF_BITS        8

#define UART_RX_BUF     2048

static struct {
    source_t          source;
    SemaphoreHandle_t tx_mtx;            // serialize op_tx / line-coding callers
    volatile bool     ready;
    serialcfg_lc_t    lc;                // current line coding (display shadow)
    uint8_t           lc_source;         // 0 default / 1 nvs / 2 rfc2217
    uint32_t          rx_bytes;
    uint32_t          tx_bytes;
    char              describe_buf[48];
} S;

#ifdef UART_MODULE_ZB_NCP
// ── Radio-Reboot-Erkennung ────────────────────────────────────────────────
// Das Radio ist ein ESP32-H2; sein ROM-Bootloader schreibt beim Start
// "ESP-ROM:esp32h2-…" auf U0TXD — auf diesem Board unvermeidbar, weil der
// ROM-Print-Strap (H2-GPIO8) an einem Pin-Header hängt und nicht gezogen ist.
// Dieser Text ist einerseits Müll im NCP-Stream, andererseits ein zuverlässiges
// Signal: das Radio hat neu gebootet, jeder Host-Zustand darüber ist veraltet.
//
// Wir nutzen ihn, um die TCP-Clients zu trennen — sonst bleibt zigbee-herdsmans
// `inReset` hängen und permit_join ist danach wirkungslos (Details in
// sink_tcp.h).  Der Scanner läuft als Zustandsautomat über die Chunk-Grenzen
// hinweg, weil der Banner beliebig zerteilt ankommen kann.
static const char ROM_BANNER[] = "ESP-ROM:";
#define ROM_BANNER_LEN  (sizeof(ROM_BANNER) - 1)
#define REBOOT_DEBOUNCE_MS  2000

static void scan_for_radio_reboot(const uint8_t *buf, size_t len)
{
    static uint8_t  matched;      // wie viele Banner-Zeichen bisher passen
    static int64_t  last_hit_us;

    for (size_t i = 0; i < len; i++) {
        if (buf[i] == (uint8_t)ROM_BANNER[matched]) {
            if (++matched < ROM_BANNER_LEN) continue;
            matched = 0;
            const int64_t now = esp_timer_get_time();
            // Ein Boot-Loop feuert den Banner im Sekundentakt; einmal trennen
            // reicht, danach hängt ohnehin niemand mehr dran.
            if (now - last_hit_us < REBOOT_DEBOUNCE_MS * 1000) continue;
            last_hit_us = now;
            ESP_LOGW(TAG, "radio ROM banner seen — radio rebooted");
            sink_tcp_drop_clients("radio rebooted");
        } else {
            // Fehlversuch: nicht stumpf auf 0 — das Zeichen kann der Anfang
            // eines neuen Treffers sein (z.B. "EE" vor "ESP-ROM:").
            matched = (buf[i] == (uint8_t)ROM_BANNER[0]) ? 1 : 0;
        }
    }
}
#endif // UART_MODULE_ZB_NCP

// ── CDC line-coding form → IDF UART enums ─────────────────────────────────
static uart_word_length_t to_word_len(uint8_t bits) {
    switch (bits) {
    case 5:  return UART_DATA_5_BITS;
    case 6:  return UART_DATA_6_BITS;
    case 7:  return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}
static uart_parity_t to_parity(uint8_t parity) {
    switch (parity) {
    case 1:  return UART_PARITY_ODD;
    case 2:  return UART_PARITY_EVEN;
    default: return UART_PARITY_DISABLE;   // 0 N (3 M / 4 S unsupported → none)
    }
}
static uart_stop_bits_t to_stop(uint8_t stop) {
    return stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;   // 0=1 / 1=1.5→1 / 2=2
}

// Apply line coding to the wire + update the display shadow.  Serialized with
// op_tx via tx_mtx.  src: 0 default / 1 nvs / 2 rfc2217.  Internal helper; the
// vtable entry is op_apply_line_coding (takes a source_t*).
static esp_err_t apply_lc(uint32_t baud, uint8_t bits, uint8_t parity,
                          uint8_t stop, uint8_t src)
{
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    esp_err_t err = uart_set_baudrate(UART_TCM_PORT, baud);
    if (err == ESP_OK) err = uart_set_word_length(UART_TCM_PORT, to_word_len(bits));
    if (err == ESP_OK) err = uart_set_parity(UART_TCM_PORT, to_parity(parity));
    if (err == ESP_OK) err = uart_set_stop_bits(UART_TCM_PORT, to_stop(stop));
    if (err == ESP_OK) {
        S.lc = (serialcfg_lc_t){ .baud = baud, .bits = bits,
                                 .parity = parity, .stop = stop };
        S.lc_source = src;
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

// ── RX: blocking read → bridge fanout (no parsing, fully transparent) ──────
static void uart_rx_task(void *arg)
{
    (void)arg;
    static uint8_t buf[256];
    while (1) {
        int rd = uart_read_bytes(UART_TCM_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (rd <= 0) continue;
        S.rx_bytes += (uint32_t)rd;
#ifdef UART_MODULE_ZB_NCP
        scan_for_radio_reboot(buf, (size_t)rd);
#endif
        if (S.source.rx_sink) {
            S.source.rx_sink(S.source.rx_sink_ctx, buf, (size_t)rd);
        }
    }
}

// ── source_t hooks ─────────────────────────────────────────────────────────

static esp_err_t op_tx(source_t *src, const uint8_t *data, size_t len)
{
    (void)src;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    if (!S.ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    int wrote = uart_write_bytes(UART_TCM_PORT, data, len);
    if (wrote == (int)len) S.tx_bytes += (uint32_t)len;
    xSemaphoreGive(S.tx_mtx);
    return wrote == (int)len ? ESP_OK : ESP_FAIL;
}

static bool op_ready(source_t *src) { (void)src; return S.ready; }

// Reset: re-pulse the module RST line if wired (best-effort).
static esp_err_t op_reset(source_t *src)
{
    (void)src;
#if UART_TCM_RST >= 0
    gpio_set_level(UART_TCM_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(UART_TCM_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(250));
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static const char *op_describe(source_t *src)
{
    (void)src;
    snprintf(S.describe_buf, sizeof(S.describe_buf), "UART " UART_MODULE_NAME " %u %u%c%u %s",
             (unsigned)S.lc.baud, S.lc.bits,
             "NOEMS"[S.lc.parity <= 4 ? S.lc.parity : 0],
             S.lc.stop == 0 ? 1 : 2, S.ready ? "ready" : "init");
    return S.describe_buf;
}

// ── Line-coding ops (RFC2217 Layer B + per-device NVS default) ─────────────
static esp_err_t op_set_line_coding(source_t *src, uint32_t baud, uint8_t bits,
                                    uint8_t parity, uint8_t stop)
{
    (void)src;
    return apply_lc(baud, bits, parity, stop, 2 /* rfc2217 */);
}

static esp_err_t op_apply_line_coding(source_t *src, uint32_t baud, uint8_t bits,
                                      uint8_t parity, uint8_t stop, uint8_t lc_src)
{
    (void)src;
    return apply_lc(baud, bits, parity, stop, lc_src);
}

// revert: RFC2217 controller released → re-resolve the NVS entry (else the
// UART_TCM_BAUD default) and re-apply, so a following raw client gets the
// device's configured rate, not the controller's leftover baud.
static void op_revert_line_coding(source_t *src)
{
    (void)src;
    serialcfg_lc_t scl;
    uint8_t lc_src;
    if (serialcfg_lookup(UART_KEY, &scl)) {
        lc_src = 1;
    } else {
        scl = (serialcfg_lc_t){ .baud = UART_DEF_BAUD, .bits = UART_DEF_BITS,
                                .parity = UART_DEF_PARITY, .stop = UART_DEF_STOP };
        lc_src = 0;
    }
    apply_lc(scl.baud, scl.bits, scl.parity, scl.stop, lc_src);
}

static void op_get_line_coding(source_t *src, uint32_t *baud, uint8_t *bits,
                               uint8_t *parity, uint8_t *stop)
{
    (void)src;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    if (baud)   *baud   = S.lc.baud;
    if (bits)   *bits   = S.lc.bits;
    if (parity) *parity = S.lc.parity;
    if (stop)   *stop   = S.lc.stop;
    xSemaphoreGive(S.tx_mtx);
}

static void op_get_stats(source_t *src, source_stats_t *out)
{
    (void)src;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = S.ready;
    out->rx_bytes  = S.rx_bytes;
    out->tx_bytes  = S.tx_bytes;
    snprintf(out->manuf,   sizeof(out->manuf),   "busware");
    snprintf(out->product, sizeof(out->product), "%s", UART_MODULE_LABEL);
}

static void op_get_serial_info(source_t *src, source_serial_info_t *out)
{
    (void)src;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = S.ready;
    out->is_vcp    = true;            // a real UART → baud is physical
    out->baud      = S.lc.baud;
    out->bits      = S.lc.bits;
    out->parity    = S.lc.parity;
    out->stop      = S.lc.stop;
    out->lc_source = S.lc_source;
    snprintf(out->key, sizeof(out->key), "%s", UART_KEY);
}

static const struct source_ops s_ops = {
    .tx                 = op_tx,
    .ready              = op_ready,
    .reset              = op_reset,
    .describe           = op_describe,
    .set_line_coding    = op_set_line_coding,
    .revert_line_coding = op_revert_line_coding,
    .get_line_coding    = op_get_line_coding,
    .apply_line_coding  = op_apply_line_coding,
    .get_stats          = op_get_stats,
    .get_serial_info    = op_get_serial_info,
};

// Bring the radio module up: SET low (operational baud), pulse RST active-low.
static void radio_bringup(void)
{
#if UART_TCM_SET >= 0
    gpio_config_t set_io = {
        .pin_bit_mask = 1ULL << UART_TCM_SET,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&set_io);
    gpio_set_level(UART_TCM_SET, 0);          // LOW = 460800 operational mode
#endif
#if UART_TCM_RST >= 0
    gpio_config_t rst_io = {
        .pin_bit_mask = 1ULL << UART_TCM_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_io);
    gpio_set_level(UART_TCM_RST, 0);          // assert reset (active-low)
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(UART_TCM_RST, 1);          // release
    vTaskDelay(pdMS_TO_TICKS(250));           // datasheet settle
#endif
}

source_t *source_uart_init(void)
{
    memset(&S, 0, sizeof(S));
    S.tx_mtx          = xSemaphoreCreateMutex();
    S.source.ops      = &s_ops;
    S.source.short_id = "uart";

    // Resolve the device's baud: per-device NVS entry, else the operational
    // default (NOT serialcfg_default()'s 115200 8N1 — EUL/TCM515 runs
    // 460800 8N1, TUL/NCN5130 runs 38400 8E1; see the module profile above).
    serialcfg_lc_t scl;
    if (serialcfg_lookup(UART_KEY, &scl)) {
        S.lc_source = 1;
    } else {
        scl = (serialcfg_lc_t){ .baud = UART_DEF_BAUD, .bits = UART_DEF_BITS,
                                .parity = UART_DEF_PARITY, .stop = UART_DEF_STOP };
        S.lc_source = 0;
    }
    S.lc = scl;

    radio_bringup();

    const uart_config_t uc = {
        .baud_rate  = (int)scl.baud,
        .data_bits  = to_word_len(scl.bits),
        .parity     = to_parity(scl.parity),
        .stop_bits  = to_stop(scl.stop),
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_TCM_PORT, UART_RX_BUF, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_TCM_PORT, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UART_TCM_PORT, UART_TCM_TX, UART_TCM_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(UART_TCM_PORT);

    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);
    S.ready = true;

    ESP_LOGI(TAG, "UART source up: port=%d tx=%d rx=%d rst=%d set=%d %u %u%c%u — source ready",
             (int)UART_TCM_PORT, UART_TCM_TX, UART_TCM_RX, UART_TCM_RST, UART_TCM_SET,
             (unsigned)S.lc.baud, S.lc.bits,
             "NOEMS"[S.lc.parity <= 4 ? S.lc.parity : 0], S.lc.stop == 0 ? 1 : 2);
    return &S.source;
}
