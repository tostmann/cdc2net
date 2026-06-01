// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb.c — generic USB-host CDC-ACM source for CDC2NET.
//
// Strips the RFNETHM source_usb down to a transparent byte pipe: no CP210x
// vendor init, no HomeMatic boot-probe, no hmu_frame.  Opens the first
// CDC-ACM-compliant device (CDC_HOST_ANY_VID/PID), asserts DTR+RTS, and:
//   - RX (bulk IN from the CUL) -> source->rx_sink() -> bridge fanout to sinks
//   - TX (from a sink, via bridge_tx_to_source -> op_tx) -> bulk OUT to the CUL
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

static const char *TAG = "src-usb";

static struct {
    source_t          source;
    cdc_acm_dev_hdl_t cdc;             // current handle, NULL when closed
    SemaphoreHandle_t disconnect_sem;
    SemaphoreHandle_t tx_mtx;          // serialize op_tx() callers
    volatile bool     ready;
    volatile bool     connected;
    uint16_t          vid;
    uint16_t          pid;
    char              manuf[48];
    char              product[48];
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
    const usb_device_desc_t *d = NULL;
    if (usb_host_get_device_descriptor(usb_dev, &d) == ESP_OK && d) {
        S.vid = d->idVendor;
        S.pid = d->idProduct;
    }
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) == ESP_OK) {
        str_desc_ascii(info.str_desc_manufacturer, S.manuf,   sizeof(S.manuf));
        str_desc_ascii(info.str_desc_product,      S.product, sizeof(S.product));
    }
    ESP_LOGW(TAG, "USB device VID=0x%04X PID=0x%04X manuf='%s' product='%s'",
             S.vid, S.pid, S.manuf, S.product);
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
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
        cdc_acm_dev_hdl_t cdc = NULL;
        esp_err_t err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID,
                                          0, &cfg, &cdc);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        S.cdc       = cdc;
        S.connected = true;
        // Standard CDC-ACM control: assert DTR+RTS (some devices gate TX on
        // DTR).  Line coding is intentionally left alone — culfw ignores the
        // USB baud rate.
        if (cdc_acm_host_set_control_line_state(cdc, true, true) != ESP_OK)
            ESP_LOGW(TAG, "set_control_line_state not supported");
        S.ready = true;
        ESP_LOGW(TAG, "CUL open (VID=0x%04X PID=0x%04X) — source ready",
                 S.vid, S.pid);

        // Block until the device disconnects; RX flows via data_cb meanwhile.
        xSemaphoreTake(S.disconnect_sem, portMAX_DELAY);
        S.ready     = false;
        S.connected = false;
        S.cdc       = NULL;
        cdc_acm_host_close(cdc);
        ESP_LOGI(TAG, "CUL closed; waiting for reconnect");
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
    .tx       = op_tx,
    .ready    = op_ready,
    .reset    = op_reset,
    .describe = op_describe,
};

source_t *source_usb_init(void)
{
    memset(&S, 0, sizeof(S));
    S.disconnect_sem  = xSemaphoreCreateBinary();
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
