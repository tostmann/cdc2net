// SPDX-License-Identifier: GPL-2.0-or-later
//
// hubprobe.c — THROWAWAY USB-host channel-budget probe for CDC2NET on ESP32-S3.
//
// Purpose: prove (or refute) where the ESP32-S3's 8 DWC host-channel budget
// wall falls when a USB hub carries TWO CDC devices (CUL 03EB:204B + C6
// USB-Serial-JTAG 303A:1001).  It enables external-hub support, enumerates
// everything behind the hub, then tries to OPEN every CDC device and KEEPS the
// handles open so channels accumulate.  Expected: hub=2ch + each CDC-with-notif
// =4ch → hub + 2×CDC = 10 > 8 → the SECOND CDC open fails with a channel-
// exhaustion error ("No more HCD channels available" from the HAL).
//
// NOT part of the product build — the whole file is gated on CDC2NET_HUBPROBE
// and only main.c's app_main calls hubprobe_run() under the same macro.

#ifdef CDC2NET_HUBPROBE

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "hubprobe.h"

static const char *TAG = "hubprobe";

#define MAX_DEV 6

typedef struct {
    uint8_t           addr;
    uint8_t           cls;          // bDeviceClass (0x09 = hub → skip open)
    uint16_t          vid, pid;
    char              product[40];
    bool              opened;
    esp_err_t         open_err;
    cdc_acm_dev_hdl_t hdl;
} dev_t;

static dev_t            s_dev[MAX_DEV];
static volatile int     s_ndev;
static SemaphoreHandle_t s_evt;      // given on each new enumeration

static void str_ascii(const usb_str_desc_t *sd, char *out, size_t cap)
{
    if (!out || !cap) return;
    out[0] = 0;
    if (!sd || sd->bLength < 2) return;
    int n = (sd->bLength - 2) / 2, o = 0;
    for (int i = 0; i < n && o + 1 < (int)cap; i++) {
        uint16_t c = sd->wData[i];
        if (!c) break;
        out[o++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    out[o] = 0;
}

// USB-host context: record addr/vid/pid/class/product.  The handle is valid
// ONLY during this callback; a CDC device must NOT be opened here.
static void new_dev_cb(usb_device_handle_t dev)
{
    if (s_ndev >= MAX_DEV) {
        ESP_LOGW(TAG, "ENUM overflow (>%d devices) — ignoring", MAX_DEV);
        return;
    }
    dev_t *d = &s_dev[s_ndev];
    memset(d, 0, sizeof(*d));
    const usb_device_desc_t *dd = NULL;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd) {
        d->vid = dd->idVendor;
        d->pid = dd->idProduct;
        d->cls = dd->bDeviceClass;
    }
    usb_device_info_t info;
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        d->addr = info.dev_addr;
        str_ascii(info.str_desc_product, d->product, sizeof(d->product));
    }
    s_ndev++;
    ESP_LOGW(TAG, "ENUM #%d: addr=%d class=0x%02X VID=0x%04X PID=0x%04X '%s'",
             s_ndev, d->addr, d->cls, d->vid, d->pid, d->product);
    if (s_evt) xSemaphoreGive(s_evt);
}

static bool data_cb(const uint8_t *data, size_t len, void *arg)
{
    (void)data; (void)len; (void)arg;
    return true;   // count-only sink; keep quiet
}

static void event_cb(const cdc_acm_host_dev_event_data_t *e, void *ctx)
{
    (void)ctx;
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED)
        ESP_LOGW(TAG, "a device disconnected");
}

static void usb_lib_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

// Open every not-yet-open CDC device, keeping handles open so DWC channels
// accumulate.  Hubs (class 0x09) are skipped — the hub driver already consumed
// their channels at enumeration.
static void try_open_all(void)
{
    for (int i = 0; i < s_ndev; i++) {
        dev_t *d = &s_dev[i];
        if (d->opened) continue;
        if (d->cls == 0x09) {                 // USB hub — not a CDC target
            ESP_LOGI(TAG, "skip addr=%d (hub class 0x09)", d->addr);
            d->opened = true;                 // mark handled so we don't retry
            continue;
        }
        cdc_acm_host_open_config_t oc = {
            .vid = d->vid, .pid = d->pid,
            .interface_idx = 0,
            .dev_addr = d->addr,              // address-aware open (2.4.0)
            .connection_timeout_ms = 5000,
            .out_buffer_size = 512,
            .in_buffer_size  = 512,
            .event_cb = event_cb,
            .data_cb  = data_cb,
        };
        uint32_t heap0 = esp_get_free_heap_size();
        esp_err_t err = cdc_acm_host_open_v2(&oc, &d->hdl);
        d->open_err = err;
        if (err == ESP_OK) {
            d->opened = true;
            ESP_LOGW(TAG, ">>> OPEN OK   addr=%d VID=0x%04X PID=0x%04X  heap %u->%u  [handle kept OPEN]",
                     d->addr, d->vid, d->pid, (unsigned)heap0,
                     (unsigned)esp_get_free_heap_size());
        } else {
            ESP_LOGE(TAG, ">>> OPEN FAIL addr=%d VID=0x%04X PID=0x%04X  err=%s (0x%x)  <<< CHANNEL WALL?",
                     d->addr, d->vid, d->pid, esp_err_to_name(err), (int)err);
        }
    }
    int opened = 0, cdc = 0;
    for (int i = 0; i < s_ndev; i++) {
        if (s_dev[i].cls == 0x09) continue;
        cdc++;
        if (s_dev[i].opened && s_dev[i].open_err == ESP_OK) opened++;
    }
    ESP_LOGW(TAG, "SUMMARY: enumerated=%d  CDC-targets=%d  CDC-opened=%d  free_heap=%u",
             s_ndev, cdc, opened, (unsigned)esp_get_free_heap_size());
}

static void probe_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "waiting for enumerations behind the hub...");
    int last = -1, stable = 0;
    for (int t = 0; t < 40; t++) {                     // up to ~20 s
        xSemaphoreTake(s_evt, pdMS_TO_TICKS(500));
        if (s_ndev == last) { if (++stable >= 6) break; }  // ~3 s quiet
        else { last = s_ndev; stable = 0; }
    }
    ESP_LOGW(TAG, "enumeration settled: %d device(s). Opening each (channels accumulate)...",
             s_ndev);
    try_open_all();

    while (1) {                                         // keep handles open; catch late arrivals
        if (xSemaphoreTake(s_evt, pdMS_TO_TICKS(5000)) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            try_open_all();
        } else {
            ESP_LOGI(TAG, "alive: enumerated=%d free_heap=%u",
                     s_ndev, (unsigned)esp_get_free_heap_size());
        }
    }
}

void hubprobe_run(void)
{
    printf("\n");
    printf("==================================================\n");
    printf("  CDC2NET HUBPROBE  —  S3 USB-host channel-budget test\n");
    printf("  S3 DWC = 8 host channels.  hub = 2ch; CDC w/notif = 4ch.\n");
    printf("  Predict: hub + 2x CDC = 10 > 8  ->  2nd CDC OPEN fails.\n");
#ifdef CONFIG_USB_HOST_HUBS_SUPPORTED
    printf("  hub support: ENABLED\n");
#else
    printf("  hub support: *** DISABLED *** (rebuild w/ HUBS_SUPPORTED=y!)\n");
#endif
    printf("==================================================\n\n");

    s_evt = xSemaphoreCreateCounting(16, 0);

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

    xTaskCreate(probe_task, "probe", 6144, NULL, 4, NULL);
    ESP_LOGW(TAG, "hubprobe up — USB host + CDC-ACM installed");
}

#endif // CDC2NET_HUBPROBE
