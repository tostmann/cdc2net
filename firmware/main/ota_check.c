// SPDX-License-Identifier: GPL-2.0-or-later
//
// ota_check — Implementation.  Siehe ota_check.h für die API-Semantik.

#include "ota_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"   // ESP_ERR_OTA_VALIDATE_FAILED
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "version.h"

static const char *TAG = "ota_check";

#define MAX_RESP_BYTES   2048   // manifest.json ist ~500 B — 2K reicht mit headroom
#define VER_STR_MAX      24
#define OTA_INSTALL_TIMEOUT_S 300   // wall-clock cap on the download loop: perform()
                                    // returns IN_PROGRESS on a read EAGAIN too, so a server
                                    // that stalls without closing the socket would otherwise
                                    // spin the loop forever with s_install stuck at "running".

typedef struct {
    ota_check_state_t state;
    char              current_ver[VER_STR_MAX];
    char              latest_ver[VER_STR_MAX];
    char              error[96];
    int64_t           last_check_us;   // 0 = nie
} ota_state_t;

static ota_state_t      s_state;
static SemaphoreHandle_t s_mtx = NULL;

// Lazy-init der Mutex (kein eigener init() — wird beim ersten
// Aufruf erzeugt, das ist racy aber funktioniert weil Handler beim
// httpd-Startup-init alle vom selben Task aufgerufen werden bevor
// die WebUI hochgeht).
static void mtx_ensure(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        // Default-state — current_ver wird beim ersten refresh() gesetzt.
        s_state.state = OTA_CHECK_IDLE;
        snprintf(s_state.current_ver, sizeof(s_state.current_ver),
                 "%s", FW_VERSION_STRING);
        s_state.latest_ver[0] = '\0';
        s_state.error[0]      = '\0';
        s_state.last_check_us = 0;
    }
}

// ───── Versions-Vergleich ──────────────────────────────────────────────

// Parsing-Helper: "0.14.138" → 3 ints, true bei Erfolg.
static bool parse_ver(const char *s, int *maj, int *min, int *bld)
{
    if (!s) return false;
    return sscanf(s, "%d.%d.%d", maj, min, bld) == 3;
}

// returnt:  >0 falls a>b,  0 falls a==b,  <0 falls a<b.  Bei Parse-Fehler 0.
static int cmp_ver(const char *a, const char *b)
{
    int am, an, ab, bm, bn, bb;
    if (!parse_ver(a, &am, &an, &ab)) return 0;
    if (!parse_ver(b, &bm, &bn, &bb)) return 0;
    if (am != bm) return (am > bm) ? 1 : -1;
    if (an != bn) return (an > bn) ? 1 : -1;
    if (ab != bb) return (ab > bb) ? 1 : -1;
    return 0;
}

// ───── HTTPS-Pull-Helper ───────────────────────────────────────────────

typedef struct {
    char    buf[MAX_RESP_BYTES + 1];
    size_t  used;
    bool    overflow;
} resp_accum_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_accum_t *a = (resp_accum_t *)evt->user_data;
    if (!a) return ESP_OK;
    if (a->overflow) return ESP_OK;
    if (a->used + evt->data_len > MAX_RESP_BYTES) {
        a->overflow = true;
        return ESP_OK;
    }
    memcpy(a->buf + a->used, evt->data, evt->data_len);
    a->used += evt->data_len;
    a->buf[a->used] = '\0';
    return ESP_OK;
}

// ───── Public API ──────────────────────────────────────────────────────

esp_err_t ota_check_refresh(void)
{
    mtx_ensure();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.state    = OTA_CHECK_CHECKING;
    s_state.error[0] = '\0';
    xSemaphoreGive(s_mtx);

    resp_accum_t accum = {0};

    esp_http_client_config_t cfg = {
        .url             = OTA_CHECK_MANIFEST_URL,
        .timeout_ms      = 6000,
        .event_handler   = http_event_cb,
        .user_data       = &accum,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "client_init failed");
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    esp_err_t e = esp_http_client_perform(cli);
    int code   = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (e != ESP_OK) {
        ESP_LOGW(TAG, "fetch failed: %s", esp_err_to_name(e));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "fetch: %s", esp_err_to_name(e));
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return e;
    }
    if (code != 200) {
        ESP_LOGW(TAG, "manifest HTTP %d", code);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "HTTP %d", code);
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    if (accum.overflow || accum.used == 0) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error),
                 accum.overflow ? "response too large" : "empty response");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(accum.buf);
    if (!root) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "JSON parse failed");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }
    cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsString(ver) || !ver->valuestring) {
        cJSON_Delete(root);
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        s_state.state = OTA_CHECK_ERROR;
        snprintf(s_state.error, sizeof(s_state.error), "no version field");
        s_state.last_check_us = esp_timer_get_time();
        xSemaphoreGive(s_mtx);
        return ESP_FAIL;
    }

    char latest[VER_STR_MAX];
    snprintf(latest, sizeof(latest), "%s", ver->valuestring);
    cJSON_Delete(root);

    int rel = cmp_ver(latest, FW_VERSION_STRING);
    ota_check_state_t new_state = (rel > 0) ? OTA_CHECK_AVAILABLE
                                            : OTA_CHECK_UP_TO_DATE;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_state.state = new_state;
    snprintf(s_state.latest_ver, sizeof(s_state.latest_ver), "%s", latest);
    snprintf(s_state.current_ver, sizeof(s_state.current_ver), "%s", FW_VERSION_STRING);
    s_state.error[0]      = '\0';
    s_state.last_check_us = esp_timer_get_time();
    xSemaphoreGive(s_mtx);

    ESP_LOGI(TAG, "manifest version=%s (current=%s) → %s",
             latest, FW_VERSION_STRING,
             (new_state == OTA_CHECK_AVAILABLE) ? "UPDATE_AVAILABLE" : "up-to-date");
    return ESP_OK;
}

// ───── HTTPS-Pull-OTA (install the release image from the server) ───────
static volatile int s_install = 0;   // 0 idle, 1 running, 2 error
static char         s_install_err[96];

// Sets the install error + state under s_mtx, so a concurrent /api/update/check
// poll (ota_check_status_json) can't serialize a torn s_install_err string.
static void install_set_err(const char *msg)
{
    mtx_ensure();
    ESP_LOGE(TAG, "OTA install failed: %s", msg);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    snprintf(s_install_err, sizeof(s_install_err), "%s", msg);
    s_install = 2;
    xSemaphoreGive(s_mtx);
}

// Cross-flash guard.  IDF's esp_https_ota_perform() already runs
// esp_ota_verify_chip_id() on the first chunk → a wrong-CHIP image (C3/C6 onto
// an S3 or vice versa) returns ESP_ERR_INVALID_VERSION *before* any flash
// write, so it can never brick: the boot partition is left untouched and the
// device keeps running the old firmware.  What IDF does NOT check is
// project_name, so a *different app for the same chip* would otherwise install.
// We therefore use the granular API to (1) reject a foreign project_name up
// front and (2) surface a chip/rev mismatch as a clear, distinct error instead
// of a bare error code.  Teardown follows the IDF advanced_https_ota example:
// esp_https_ota_abort() on every pre-finish failure; never abort after finish().
static void ota_install_task(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "OTA install: pulling %s", OTA_FIRMWARE_URL);
    esp_http_client_config_t http = {
        .url               = OTA_FIRMWARE_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota = { .http_config = &http };

    esp_https_ota_handle_t h = NULL;
    esp_err_t e = esp_https_ota_begin(&ota, &h);
    if (e != ESP_OK || h == NULL) {
        install_set_err(esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }

    // Guard 1 — project_name must match our own running firmware.  Reading the
    // descriptor does not write flash, so an abort here costs nothing.
    esp_app_desc_t img = {0};
    e = esp_https_ota_get_img_desc(h, &img);
    if (e != ESP_OK) {
        esp_https_ota_abort(h);
        install_set_err("image header unreadable");
        vTaskDelete(NULL);
        return;
    }
    const esp_app_desc_t *self = esp_app_get_description();
    if (strncmp(img.project_name, self->project_name, sizeof(img.project_name)) != 0) {
        char m[96];
        // project_name[] is not guaranteed NUL-terminated → bound the prints.
        snprintf(m, sizeof(m), "foreign image '%.*s' (expected '%.*s')",
                 (int)sizeof(img.project_name), img.project_name,
                 (int)sizeof(self->project_name), self->project_name);
        ESP_LOGE(TAG, "OTA reject: %s", m);
        esp_https_ota_abort(h);
        install_set_err(m);
        vTaskDelete(NULL);
        return;
    }

    // Download loop.  The first perform() runs IDF's chip_id + chip_rev verify;
    // a wrong-chip image aborts here (ESP_ERR_INVALID_VERSION) with nothing
    // committed to the boot partition.  Bounded by a wall-clock deadline because
    // perform() returns IN_PROGRESS on a read timeout (EAGAIN) too.
    const int64_t deadline = esp_timer_get_time() + (int64_t)OTA_INSTALL_TIMEOUT_S * 1000000;
    while ((e = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (esp_timer_get_time() > deadline) { e = ESP_ERR_TIMEOUT; break; }
        /* streaming the image to the passive OTA partition */
    }
    if (e != ESP_OK) {
        esp_https_ota_abort(h);
        const char *msg;
        if (e == ESP_ERR_INVALID_VERSION)   // chip_id OR chip_revision mismatch
            msg = "incompatible image (chip_id or chip_revision mismatch)";
        else if (e == ESP_ERR_TIMEOUT)
            msg = "download stalled (timed out)";
        else
            msg = esp_err_to_name(e);
        install_set_err(msg);
        vTaskDelete(NULL);
        return;
    }
    if (!esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        install_set_err("incomplete download");
        vTaskDelete(NULL);
        return;
    }

    // finish() does the final full-image validation + sets the boot partition.
    // It consumes the handle → do NOT call abort after this point.
    e = esp_https_ota_finish(h);
    if (e != ESP_OK) {
        install_set_err(e == ESP_ERR_OTA_VALIDATE_FAILED
                            ? "image validation failed (corrupt)"
                            : esp_err_to_name(e));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "OTA install OK — rebooting in 500 ms");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// Spawn the install in the background.  Returns ESP_OK if started.  Pulls our
// own release server; ota_install_task guards project_name and relies on IDF's
// chip_id verify, so a cross-chip image can never be committed to boot.
esp_err_t ota_install_start(void)
{
    mtx_ensure();
    if (s_install == 1) return ESP_ERR_INVALID_STATE;   // already running (single web task)
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_install_err[0] = '\0';
    s_install = 1;
    xSemaphoreGive(s_mtx);
    BaseType_t r = xTaskCreate(ota_install_task, "ota_install", 8192, NULL, 5, NULL);
    if (r != pdPASS) {
        install_set_err("task spawn failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const char *state_name(ota_check_state_t s)
{
    switch (s) {
        case OTA_CHECK_IDLE:        return "idle";
        case OTA_CHECK_CHECKING:    return "checking";
        case OTA_CHECK_UP_TO_DATE:  return "up_to_date";
        case OTA_CHECK_AVAILABLE:   return "available";
        case OTA_CHECK_ERROR:       return "error";
    }
    return "?";
}

int ota_check_status_json(char *buf, size_t cap)
{
    mtx_ensure();

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ota_state_t snap = s_state;
    int  install_snap = s_install;
    char install_err_snap[sizeof(s_install_err)];
    memcpy(install_err_snap, s_install_err, sizeof(s_install_err));
    xSemaphoreGive(s_mtx);

    int age_s = -1;
    if (snap.last_check_us > 0) {
        int64_t now_us = esp_timer_get_time();
        age_s = (int)((now_us - snap.last_check_us) / 1000000);
        if (age_s < 0) age_s = 0;
    }

    int n = snprintf(buf, cap,
        "{\"state\":\"%s\","
        "\"current\":\"%s\","
        "\"available\":\"%s\","
        "\"update_available\":%s,"
        "\"checked_age_s\":%d,"
        "\"error\":\"%s\","
        "\"install\":\"%s\",\"install_err\":\"%s\"}",
        state_name(snap.state),
        snap.current_ver,
        snap.latest_ver,
        (snap.state == OTA_CHECK_AVAILABLE) ? "true" : "false",
        age_s,
        snap.error,
        install_snap == 1 ? "running" : install_snap == 2 ? "error" : "idle",
        install_err_snap);

    return (n < 0 || (size_t)n >= cap) ? -1 : n;
}
