// SPDX-License-Identifier: GPL-2.0-or-later
//
// mission_tbr.c — experimental on-device Thread Border Router (single ESP32-C6
// native 802.15.4 radio, no RCP), gated on CONFIG_CDC2NET_MISSION_TBR.
//
// A full OpenThread Border Router whose IP backbone is the existing W5500
// ethernet netif (net_eth.c), running ALONGSIDE the EUL/TUL UART-source bridge.
// Init sequence mirrors the IDF ot_br example (esp_ot_br.c + ot_examples_br.c +
// ot_network.c), with example_connect()/get_example_netif() replaced by our own
// W5500 netif as the Thread backbone.
//
// Build note: enabling CONFIG_CDC2NET_MISSION_TBR pulls in OpenThread + full
// lwIP IPv6 forwarding + mbedtls DTLS/ECJPAKE — append sdkconfig.defaults.tbr to
// the build's SDKCONFIG_DEFAULTS for the required Kconfig values (the W5500
// netif also needs CONFIG_LWIP_IPV6 to bring up the backbone — see net_eth.c).

#include "sdkconfig.h"
#if defined(CONFIG_CDC2NET_MISSION_TBR)

#include "mission_tbr.h"
#include "net_eth.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"

#include "esp_openthread.h"
#include "esp_openthread_types.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_netif_glue.h"

#include <openthread/instance.h>
#include <openthread/dataset.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>

static const char *TAG = "tbr";

// Default platform-config (from the IDF ot_br example esp_ot_config.h; the
// native-radio variant only — the C6 carries the 802.15.4 MAC on-die).
#define TBR_RADIO_CONFIG()  { .radio_mode = RADIO_MODE_NATIVE }
#define TBR_HOST_CONFIG()   { .host_connection_mode = HOST_CONNECTION_MODE_NONE }
#define TBR_PORT_CONFIG()   { .storage_partition_name = "nvs", \
                              .netif_queue_size = 10,          \
                              .task_queue_size  = 10 }

// Border-router init runs on its own task: it must hold the OT lock, and the
// backbone netif (our W5500) must already exist (net_init() brought it up).
static void ot_br_init_task(void *ctx)
{
    (void)ctx;
    esp_netif_t *backbone = net_eth_netif();
    if (!backbone) {
        ESP_LOGE(TAG, "no W5500 backbone netif — BR not started (W5500 absent?)");
        vTaskDelete(NULL);
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_backbone_netif(backbone);
    esp_err_t err = esp_openthread_border_router_init();
    esp_openthread_lock_release();
    ESP_LOGW(TAG, "border_router_init -> %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

// Lightweight operational visibility: Thread role + child/neighbor count.
static void tbr_observe_task(void *ctx)
{
    (void)ctx;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        esp_openthread_lock_acquire(portMAX_DELAY);
        otInstance *inst = esp_openthread_get_instance();
        otDeviceRole role = otThreadGetDeviceRole(inst);
        int children = 0; otChildInfo ci;
        for (uint16_t i = 0; otThreadGetChildInfoByIndex(inst, i, &ci) == OT_ERROR_NONE; i++) children++;
        int neighbors = 0; otNeighborInfo ni;
        otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
        while (otThreadGetNextNeighborInfo(inst, &it, &ni) == OT_ERROR_NONE) neighbors++;
        esp_openthread_lock_release();
        ESP_LOGI(TAG, "thread: role=%s children=%d neighbors=%d",
                 otThreadDeviceRoleToString(role), children, neighbors);
    }
}

void mission_tbr_start(void)
{
    ESP_LOGW(TAG, "MISSION_TBR: starting on-device OpenThread Border Router");

    // eventfds: netif + task-queue + border-router + native-radio = 4.
    esp_vfs_eventfd_config_t ev = { .max_fds = 4 };
    esp_err_t e = esp_vfs_eventfd_register(&ev);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "eventfd register failed (%s)", esp_err_to_name(e));
        return;
    }

    static esp_openthread_config_t config = {
        .netif_config    = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = TBR_RADIO_CONFIG(),
            .host_config  = TBR_HOST_CONFIG(),
            .port_config  = TBR_PORT_CONFIG(),
        },
    };
    ESP_ERROR_CHECK(esp_openthread_start(&config));   // spawns the ot_main worker

    // BR init (needs the eth backbone + OT lock) — async task, like the example.
    xTaskCreate(ot_br_init_task, "ot_br_init", 6144, NULL, 4, NULL);

    // Auto-start the Thread network from the configured dataset (or a fresh one
    // if none is stored): brings ifconfig up + thread start; becomes leader with
    // no peer present.
    otOperationalDatasetTlvs dataset;
    esp_openthread_lock_acquire(portMAX_DELAY);
    otError oterr = otDatasetGetActiveTlvs(esp_openthread_get_instance(), &dataset);
    ESP_ERROR_CHECK(esp_openthread_auto_start((oterr == OT_ERROR_NONE) ? &dataset : NULL));
    esp_openthread_lock_release();

    xTaskCreate(tbr_observe_task, "tbr_obs", 4096, NULL, 3, NULL);
}

#endif // CONFIG_CDC2NET_MISSION_TBR
