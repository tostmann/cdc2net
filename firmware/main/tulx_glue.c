// SPDX-License-Identifier: GPL-2.0-or-later
//
// TULX32 build glue: image marker and recovery entry.
//
// The TULX32 has one application slot and a recovery system in the factory
// partition; the user has no USB.  An application for this product therefore
// (1) identifies itself with the busware image marker, (2) never installs
// firmware itself, and (3) offers a way back into the recovery.
#ifdef TULX_BUILD

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "tulx_glue.h"
#include "version.h"

static const char *TAG = "tulx";

// Read by the TULX32 recovery before it installs an image.  `variant` names the
// PRODUCT — every application on a TULX32 carries variant=tulx32 — and `app`
// tells those applications apart.  An image without the marker is installable
// but needs the user to confirm, and runs without rollback protection.
__attribute__((used, retain))
static const char bwImgMarker[] =
    "BWIMG1|variant=tulx32|app=cdc2net|version=" FW_VERSION_STRING "|";

// An image carrying the busware marker is installed with otadata state NEW, so
// the bootloader puts it on probation: unconfirmed, it survives exactly ONE
// boot and the next reset lands in the recovery.  Bench 2026-09-15 showed that
// happening to this firmware ("rolled back (failed to start)").
static void confirm_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60000));
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "image confirmed: %s", esp_err_to_name(e));
    vTaskDelete(NULL);
}

void tulx_confirm_running(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;                     /* not on probation — nothing to do */
    }
    ESP_LOGW(TAG, "this image is on probation; confirming in 60 s");
    xTaskCreate(confirm_task, "ota_confirm", 2560, NULL, 3, NULL);
}

void tulx_enter_recovery(void)
{
    // factory is the only partition a running application can select, and the
    // call clears otadata; the recovery restores it when it starts.
    const esp_partition_t *f = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!f) {
        ESP_LOGE(TAG, "no factory partition — this image is not on a recovery layout");
        return;
    }
    esp_err_t e = esp_ota_set_boot_partition(f);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGW(TAG, "restarting into recovery (%s)", bwImgMarker);
    esp_restart();
}

#endif  // TULX_BUILD
