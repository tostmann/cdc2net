// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb.c — generic USB-host CDC-ACM source for CDC2NET.
//
// Strips the RFNETHM source_usb down to a transparent byte pipe: no CP210x
// vendor init, no HomeMatic boot-probe, no hmu_frame.  Waits for a device to
// enumerate, then opens it by VID: native CDC-ACM sticks (CUL/TUL/EUL) go
// through the generic byte-transparent path; the three common foreign
// USB-serial bridges (FTDI / WCH CH34x / Silabs CP210x) go through their VCP
// driver so the chip wire framing is handled (notably the FTDI 2-byte modem
// status that the bare CDC-ACM path would pass through as garbage) and the
// UART baud is actually set.  Then:
//   - RX (bulk IN from the stick) -> source->rx_sink() -> bridge fanout to sinks
//   - TX (from a sink, via bridge_tx_to_source -> op_tx) -> bulk OUT to the stick
//
// Implements the source_t interface; the bridge wires rx_sink on attach.

#include "source_usb.h"
#include "bridge.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ftdi.h"     // ftdi_vcp_open   — FTDI FT23x (status-byte strip + baud)
#include "usb/vcp_ch34x.h"    // ch34x_vcp_open  — WCH CH340/CH341
#include "usb/vcp_cp210x.h"   // cp210x_vcp_open — Silabs CP210x
#include "serialcfg.h"        // per-device line-coding store (Layer A)

static const char *TAG = "src-usb";

static struct {
    source_t          source;
    cdc_acm_dev_hdl_t cdc;             // current handle, NULL when closed
    SemaphoreHandle_t disconnect_sem;
    SemaphoreHandle_t connect_sem;     // given by new_dev_cb on enumeration
    SemaphoreHandle_t tx_mtx;          // serialize op_tx() / line-coding callers
    volatile bool     ready;
    volatile bool     connected;
    uint16_t          vid;
    uint16_t          pid;
    char              manuf[48];
    char              product[48];
    char              serial[SERIALCFG_KEY_LEN];   // USB iSerialNumber ("" if none)
    char              key[SERIALCFG_KEY_LEN];      // device key: serial else "VVVV:PPPP"
    bool              is_vcp;                       // current device opened via a VCP driver
    cdc_acm_line_coding_t current_lc;               // last-applied line coding (display shadow)
    uint8_t           lc_source;                    // 0 default / 1 nvs / 2 rfc2217
    uint32_t          rx_bytes;
    uint32_t          tx_bytes;
    char              describe_buf[48];
} S;

// ── CDC-ACM RX: hand straight to the bridge fanout ───────────────────────
static bool data_cb(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    S.rx_bytes += (uint32_t)len;
    if (S.source.rx_sink) {
        S.source.rx_sink(S.source.rx_sink_ctx, data, len);
    }
    return true;   // RX buffer can be flushed
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "CUL disconnected");
        S.ready     = false;
        S.connected = false;
        if (S.disconnect_sem) xSemaphoreGive(S.disconnect_sem);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "serial-state 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

// Convert a USB string descriptor (UTF-16LE) to a printable ASCII buffer.
static void str_desc_ascii(const usb_str_desc_t *sd, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!sd || sd->bLength < 2) return;
    int n = (sd->bLength - 2) / 2;
    int o = 0;
    for (int i = 0; i < n && o + 1 < (int)cap; i++) {
        uint16_t c = sd->wData[i];
        if (c == 0) break;
        out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[o] = '\0';
}

// New-device callback (USB Host context): record VID/PID + the manufacturer
// and product string descriptors (legacy CUL puts something useful there).
static void new_dev_cb(usb_device_handle_t usb_dev)
{
    // Single-stick design: ignore enumerations while a device is already open.
    // A concurrent/duplicate NEW_DEV would otherwise overwrite the live device's
    // VID/PID (mis-reporting /api/status + op_describe) and latch a stale
    // connect_sem give that drives a spurious open after the live device later
    // disconnects.  We only ever service one source.
    if (S.connected) return;

    const usb_device_desc_t *d = NULL;
    if (usb_host_get_device_descriptor(usb_dev, &d) == ESP_OK && d) {
        S.vid = d->idVendor;
        S.pid = d->idProduct;
    }
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) == ESP_OK) {
        str_desc_ascii(info.str_desc_manufacturer, S.manuf,   sizeof(S.manuf));
        str_desc_ascii(info.str_desc_product,      S.product, sizeof(S.product));
        // iSerialNumber can be NULL (e.g. most CH340s) — str_desc_ascii yields
        // "" then, and the device key falls back to VID:PID (see stick_task).
        str_desc_ascii(info.str_desc_serial_num,   S.serial,  sizeof(S.serial));
    }
    ESP_LOGW(TAG, "USB device VID=0x%04X PID=0x%04X manuf='%s' product='%s'",
             S.vid, S.pid, S.manuf, S.product);
    // VID/PID are now recorded — release stick_task to pick the open path.
    if (S.connect_sem) xSemaphoreGive(S.connect_sem);
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

// Open the just-enumerated device, routing the three known foreign USB-serial
// chips through their VCP driver and everything else through the generic
// CDC-ACM path.  vid/pid are a snapshot taken by the caller (not the shared
// S.vid/S.pid) so a racing enumeration can't tear the pair underneath us.
// *is_vcp tells the caller whether the chip framing/baud is owned by a driver.
static esp_err_t open_dispatch(uint16_t vid, uint16_t pid,
                               const cdc_acm_host_device_config_t *cfg,
                               cdc_acm_dev_hdl_t *cdc,
                               const char **kind, bool *is_vcp)
{
    *is_vcp = true;
    switch (vid) {
    case FTDI_VID:                       // 0x0403  FTDI FT23x
        *kind = "FTDI";
        return ftdi_vcp_open(pid, 0, cfg, cdc);
    case NANJING_QINHENG_MICROE_VID:     // 0x1A86  WCH CH340/CH341
        *kind = "CH34x";
        return ch34x_vcp_open(pid, 0, cfg, cdc);
    case SILICON_LABS_VID:               // 0x10C4  Silabs CP210x
        *kind = "CP210x";
        return cp210x_vcp_open(pid, 0, cfg, cdc);
    default:                             // native CDC-ACM (CUL/TUL/EUL)
        *is_vcp = false;
        *kind = "CDC-ACM";
        return cdc_acm_host_open(vid, pid, 0, cfg, cdc);
    }
}

// Derive the per-device key: USB iSerialNumber (sanitised) if present, else
// "VVVV:PPPP".  CH340s usually have no serial → all share the VID:PID entry.
static void derive_key(uint16_t vid, uint16_t pid, char *out, size_t cap)
{
    if (S.serial[0]) {
        size_t o = 0;
        for (const char *p = S.serial; *p && o + 1 < cap; p++) {
            char c = *p;
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            out[o++] = ok ? c : '_';
        }
        out[o] = '\0';
    } else {
        snprintf(out, cap, "%04X:%04X", vid, pid);
    }
}

// Apply line coding to the currently-open device and update the display
// shadow.  Serialized with op_tx() via tx_mtx (both touch S.cdc).  On the VCP
// path it pushes a real SET_LINE_CODING; for native CDC it only records the
// value for display (culfw ignores the USB baud).  src: 0 default/1 nvs/2 rfc2217.
esp_err_t source_usb_apply_line_coding(uint32_t baud, uint8_t bits,
                                       uint8_t parity, uint8_t stop, uint8_t src)
{
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    cdc_acm_line_coding_t lc = {
        .dwDTERate   = baud,
        .bCharFormat = stop,
        .bParityType = parity,
        .bDataBits   = bits,
    };
    esp_err_t err = ESP_OK;
    if (S.cdc && S.is_vcp) {
        err = cdc_acm_host_line_coding_set(S.cdc, &lc);
    }
    if (err == ESP_OK) {
        S.current_lc = lc;        // shadow (no line_coding_get on FTDI/CH34x)
        S.lc_source  = src;
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

static void stick_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size       = 512,
        .in_buffer_size        = 512,
        .event_cb              = event_cb,
        .data_cb               = data_cb,
        .user_arg              = NULL,
    };

    while (1) {
        // Wait until new_dev_cb has enumerated a device and recorded its
        // VID/PID, then snapshot the identity once so a racing enumeration
        // can't tear the (vid,pid) pair underneath open_dispatch().
        xSemaphoreTake(S.connect_sem, portMAX_DELAY);
        const uint16_t vid = S.vid, pid = S.pid;

        // Discard any disconnect signal left over from a previous open that
        // aborted mid-way: the VCP open registers event_cb *before* its
        // chip-init control transfers, so a yank in that window latches
        // disconnect_sem.  Drain it here or the next healthy device would be
        // torn down instantly (closed before a single byte flows).
        xSemaphoreTake(S.disconnect_sem, 0);

        cdc_acm_dev_hdl_t cdc = NULL;
        const char *kind = "?";
        bool is_vcp = false;
        esp_err_t err = open_dispatch(vid, pid, &cfg, &cdc, &kind, &is_vcp);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s open failed (VID=0x%04X PID=0x%04X): %s",
                     kind, vid, pid, esp_err_to_name(err));
            // The device may still be physically present (a transient control
            // transfer or a >timeout enumeration).  new_dev_cb fires only once
            // per physical enumeration, so re-arm the open ourselves instead of
            // blocking forever on connect_sem — restores the pre-VCP self-heal.
            vTaskDelay(pdMS_TO_TICKS(2000));
            xSemaphoreGive(S.connect_sem);
            continue;
        }
        S.cdc       = cdc;
        S.connected = true;
        S.is_vcp    = is_vcp;
        derive_key(vid, pid, S.key, sizeof(S.key));

        // Resolve this device's line coding: per-device NVS entry, else the
        // global default.  Applied on the VCP path (the real wire rate); native
        // CDC records it for display only and keeps DTR/RTS asserted.
        serialcfg_lc_t scl;
        uint8_t lc_src;
        if (serialcfg_lookup(S.key, &scl)) {
            lc_src = 1;                 // per-device NVS default
        } else {
            scl = serialcfg_default();
            lc_src = 0;                 // global fallback
        }
        if (source_usb_apply_line_coding(scl.baud, scl.bits, scl.parity,
                                         scl.stop, lc_src) != ESP_OK)
            ESP_LOGW(TAG, "line coding set failed");
        if (!is_vcp) {
            // Native CDC-ACM: assert DTR+RTS (some devices gate TX on DTR).
            if (cdc_acm_host_set_control_line_state(cdc, true, true) != ESP_OK)
                ESP_LOGW(TAG, "set_control_line_state not supported");
        }
        S.ready = true;
        ESP_LOGW(TAG, "%s open (VID=0x%04X PID=0x%04X) key='%s' %u%c%u@%u — source ready",
                 kind, vid, pid, S.key, S.current_lc.bDataBits,
                 "NOEMS"[S.current_lc.bParityType <= 4 ? S.current_lc.bParityType : 0],
                 S.current_lc.bCharFormat == 0 ? 1 : 2, (unsigned)S.current_lc.dwDTERate);

        // Block until the device disconnects; RX flows via data_cb meanwhile.
        xSemaphoreTake(S.disconnect_sem, portMAX_DELAY);
        S.ready     = false;
        S.connected = false;
        S.cdc       = NULL;
        S.is_vcp    = false;
        S.key[0]    = '\0';
        S.serial[0] = '\0';
        cdc_acm_host_close(cdc);
        ESP_LOGI(TAG, "source closed; waiting for reconnect");
        // Device is gone — tell the bridge so sinks drop per-device state.
        // The raw-TCP sink closes its client connections, so downstream
        // (FHEM) reconnects and re-initialises against whatever stick is
        // attached next.  The bridge can't tell a swap from a reconnect
        // (e.g. C3/C6 both enumerate as 303A:1001), so every disconnect
        // must invalidate the downstream session.
        bridge_notify_source_down();
    }
}

// ── source_t hooks ───────────────────────────────────────────────────────

static esp_err_t op_tx(source_t *src, const uint8_t *data, size_t len)
{
    (void)src;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    cdc_acm_dev_hdl_t cdc = S.cdc;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (cdc) {
        err = cdc_acm_host_data_tx_blocking(cdc, data, len, 500 /* ms */);
        if (err == ESP_OK) S.tx_bytes += (uint32_t)len;
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

static bool op_ready(source_t *src) { (void)src; return S.ready; }

// ── Line-coding ops (RFC2217 Layer B) ─────────────────────────────────────
// set: an RFC2217 controller issued SET-*.  Apply with src=2 (rfc2217) so the
// WebUI/NVS path knows a live session owns the wire and won't clobber it.
static esp_err_t op_set_line_coding(source_t *src, uint32_t baud, uint8_t bits,
                                    uint8_t parity, uint8_t stop)
{
    (void)src;
    return source_usb_apply_line_coding(baud, bits, parity, stop, 2 /* rfc2217 */);
}

// revert: the RFC2217 controller released.  Re-resolve this device's NVS entry
// (else the global default) and re-apply it — so a following raw client gets
// the device's configured rate, not the controller's leftover baud.  S.key is
// stable while the device stays connected (set once in stick_task).
static void op_revert_line_coding(source_t *src)
{
    (void)src;
    // Snapshot the device key under tx_mtx: stick_task clears it (S.key[0]='\0')
    // on disconnect and rewrites it via derive_key() on the next open, so an
    // unguarded read here could race a torn key with a controller-release that
    // interleaves a USB disconnect.  Release tx_mtx BEFORE the apply below —
    // tx_mtx is non-recursive and source_usb_apply_line_coding() re-takes it.
    char key[SERIALCFG_KEY_LEN];
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    snprintf(key, sizeof(key), "%s", S.key);
    xSemaphoreGive(S.tx_mtx);

    serialcfg_lc_t scl;
    uint8_t lc_src;
    if (key[0] && serialcfg_lookup(key, &scl)) {
        lc_src = 1;
    } else {
        scl = serialcfg_default();
        lc_src = 0;
    }
    source_usb_apply_line_coding(scl.baud, scl.bits, scl.parity, scl.stop, lc_src);
}

// get: read back the last-applied coding (display shadow; no GET on FTDI/CH34x).
// tx_mtx-guarded so a concurrent apply can't tear the struct read.
static void op_get_line_coding(source_t *src, uint32_t *baud, uint8_t *bits,
                               uint8_t *parity, uint8_t *stop)
{
    (void)src;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    if (baud)   *baud   = S.current_lc.dwDTERate;
    if (bits)   *bits   = S.current_lc.bDataBits;
    if (parity) *parity = S.current_lc.bParityType;
    if (stop)   *stop   = S.current_lc.bCharFormat;
    xSemaphoreGive(S.tx_mtx);
}

static esp_err_t op_reset(source_t *src)
{
    (void)src;
    // No VBUS FET on the YD-ESP32-S3 (passive 5V tie) -> no power-cycle.
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *op_describe(source_t *src)
{
    (void)src;
    snprintf(S.describe_buf, sizeof(S.describe_buf), "USB CDC %04X:%04X %s",
             S.vid, S.pid, S.connected ? "open" : "closed");
    return S.describe_buf;
}

static const struct source_ops s_ops = {
    .tx                 = op_tx,
    .ready              = op_ready,
    .reset              = op_reset,
    .describe           = op_describe,
    .set_line_coding    = op_set_line_coding,
    .revert_line_coding = op_revert_line_coding,
    .get_line_coding    = op_get_line_coding,
};

source_t *source_usb_init(void)
{
    memset(&S, 0, sizeof(S));
    S.disconnect_sem  = xSemaphoreCreateBinary();
    S.connect_sem     = xSemaphoreCreateBinary();
    S.tx_mtx          = xSemaphoreCreateMutex();
    S.source.ops      = &s_ops;
    S.source.short_id = "usb";

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 5, NULL);

    const cdc_acm_host_driver_config_t drv = {
        .driver_task_stack_size = 4096,
        .driver_task_priority   = 6,
        .xCoreID                = 0,
        .new_dev_cb             = new_dev_cb,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&drv));
    xTaskCreate(stick_task, "stick", 6144, NULL, 4, NULL);

    ESP_LOGI(TAG, "USB host + CDC-ACM source installed");
    return &S.source;
}

void source_usb_get_stats(source_usb_stats_t *out)
{
    if (!out) return;
    out->connected = S.connected;
    out->vid       = S.vid;
    out->pid       = S.pid;
    out->rx_bytes  = S.rx_bytes;
    out->tx_bytes  = S.tx_bytes;
    snprintf(out->manuf,   sizeof(out->manuf),   "%s", S.manuf);
    snprintf(out->product, sizeof(out->product), "%s", S.product);
}

void source_usb_get_serial_info(source_usb_serial_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = S.connected;
    out->is_vcp    = S.is_vcp;
    out->vid       = S.vid;
    out->pid       = S.pid;
    out->baud      = S.current_lc.dwDTERate;
    out->bits      = S.current_lc.bDataBits;
    out->parity    = S.current_lc.bParityType;
    out->stop      = S.current_lc.bCharFormat;
    out->lc_source = S.lc_source;
    snprintf(out->serial, sizeof(out->serial), "%s", S.serial);
    snprintf(out->key,    sizeof(out->key),    "%s", S.key);
}
