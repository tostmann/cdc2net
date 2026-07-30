// SPDX-License-Identifier: GPL-2.0-or-later
//
// radio_flash.c — see radio_flash.h.  Writes the embedded radio firmware to
// the companion ESP SoC over the inter-chip UART using esp-serial-flasher.

#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
// esp_app_desc_t + ESP_APP_DESC_MAGIC_WORD live in esp_app_desc.h (IDF 5.x);
// esp_app_format.h only carries the image/segment headers.
#include "esp_app_desc.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp32_port.h"
#include "esp_loader.h"

#include "esp_partition.h"
// MD5 lives in radio_md5.c: every route to a hash (mbedtls/md5.h as well as
// esp_rom_md5.h) redefines the `struct MD5Context` that esp-serial-flasher's
// public header already brings in, so it cannot share this file.  See there.
void radio_md5_hex(const uint8_t *data, size_t len, char out[33]);

#include "radio_flash.h"

static const char *TAG = "radio-fw";

static radio_info_t s_info;   // filled by radio_flash_sync(), read by the web UI

const radio_info_t *radio_flash_info(void)
{
    return &s_info;
}

// Same UART + pins the byte-transparent source uses afterwards (source_uart.c);
// overriding UART_TCM_* in the env therefore moves both consistently.
#ifndef UART_TCM_PORT
#define UART_TCM_PORT   UART_NUM_1
#endif
#ifndef UART_TCM_TX
#define UART_TCM_TX     4
#endif
#ifndef UART_TCM_RX
#define UART_TCM_RX     5
#endif

// Straps into the radio: RESET drives its EN, BOOT drives the GPIO that
// selects download mode (H2/C6/C3: GPIO9).  Board wiring on the ESP
// Thread-BR / Zigbee-GW board is S3 GPIO7 -> H2 EN, S3 GPIO8 -> H2 GPIO9.
#ifndef RADIO_FLASH_RESET_GPIO
#define RADIO_FLASH_RESET_GPIO  7
#endif
#ifndef RADIO_FLASH_BOOT_GPIO
#define RADIO_FLASH_BOOT_GPIO   8
#endif

// Sync happens at the radio's normal NCP rate; the transfer itself is raised
// once the ROM loader is talking (the ROM always syncs at 115200 first).
#ifndef RADIO_FLASH_SYNC_BAUD
#define RADIO_FLASH_SYNC_BAUD   115200
#endif
#ifndef RADIO_FLASH_XFER_BAUD
#define RADIO_FLASH_XFER_BAUD   460800
#endif

// The merged factory image places the application at 0x20000; esp_app_desc_t
// sits right behind its image header (24 B) + first segment header (8 B).
#define APP_IMAGE_ADDR      0x20000
#define APP_DESC_ADDR       (APP_IMAGE_ADDR + 0x20)
#define RADIO_PROJECT_NAME  "esp-coordinator"

#define FLASH_BLOCK_SIZE    1024

// Partition holding the radio's merged factory image (partitions_ota.csv).
#define RADIO_FW_PARTITION  "radio_fw"

// Read an esp_app_desc_t and sanity-check it.  Returns false when the magic
// does not match, which is also the answer for "radio flash is blank".
static bool desc_valid(const esp_app_desc_t *d)
{
    return d->magic_word == ESP_APP_DESC_MAGIC_WORD;
}

// Leave the straps in the state a free-running radio needs: BOOT released so
// the ROM takes SPI boot, RESET released so the board pull-up holds EN high.
// Both back to inputs — the radio must not be held in reset by a host GPIO.
static void release_straps(void)
{
    gpio_reset_pin((gpio_num_t)RADIO_FLASH_BOOT_GPIO);
    gpio_reset_pin((gpio_num_t)RADIO_FLASH_RESET_GPIO);
}

void radio_flash_sync(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, RADIO_FW_PARTITION);
    if (!part) {
        ESP_LOGE(TAG, "no '%s' partition — radio left untouched", RADIO_FW_PARTITION);
        return;
    }

    // Map it so the image can be handed to the flasher without a second copy in
    // RAM (it is ~720 KB; the S3 has nothing like that to spare).
    const void *mapped = NULL;
    esp_partition_mmap_handle_t map = 0;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &map) != ESP_OK) {
        ESP_LOGE(TAG, "cannot map '%s' — radio left untouched", RADIO_FW_PARTITION);
        return;
    }
    const uint8_t *img = (const uint8_t *)mapped;

    // The partition is bigger than the image; the tail is erased flash.  Trim it
    // back to the last non-0xFF byte and round up — the flasher wants a 4-byte
    // aligned length, and writing 0xFF padding would only cost time.
    uint32_t img_size = part->size;
    while (img_size > 0 && img[img_size - 1] == 0xFF) {
        img_size--;
    }
    img_size = (img_size + 3u) & ~3u;

    // img[0] == 0xE9 is the COMMIT MARKER of a network-staged image: /api/radio
    // streams everything from sector 1 on and writes sector 0 (which carries the
    // ESP image magic) only after the full body arrived and the app descriptor
    // verified.  An interrupted upload therefore leaves 0xFF here — and a
    // truncated-but-descriptor-valid image must never reach the radio (the
    // self-healing sync would re-flash the broken image forever).
    if (img_size <= APP_DESC_ADDR + sizeof(esp_app_desc_t) || img[0] != 0xE9) {
        ESP_LOGE(TAG, "'%s' holds no usable image (%" PRIu32 " B, first byte 0x%02X) — skipping",
                 RADIO_FW_PARTITION, img_size, img_size ? img[0] : 0xFF);
        esp_partition_munmap(map);
        return;
    }

    esp_app_desc_t want;
    memcpy(&want, img + APP_DESC_ADDR, sizeof(want));
    if (!desc_valid(&want)) {
        ESP_LOGE(TAG, "'%s' has no app descriptor — skipping", RADIO_FW_PARTITION);
        esp_partition_munmap(map);
        return;
    }
    ESP_LOGI(TAG, "staged radio fw: %s %s (%" PRIu32 " B)",
             want.project_name, want.version, img_size);
    strlcpy(s_info.staged, want.version, sizeof(s_info.staged));

    esp32_port_t port = {
        .port.ops    = &esp32_uart_ops,
        .baud_rate   = RADIO_FLASH_SYNC_BAUD,
        .uart_port   = UART_TCM_PORT,
        .uart_rx_pin = (gpio_num_t)UART_TCM_RX,
        .uart_tx_pin = (gpio_num_t)UART_TCM_TX,
        .reset_pin   = (gpio_num_t)RADIO_FLASH_RESET_GPIO,
        .boot_pin    = (gpio_num_t)RADIO_FLASH_BOOT_GPIO,
    };

    esp_loader_t loader;
    if (esp_loader_init_serial(&loader, &port.port) != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "loader init failed — leaving radio untouched");
        esp_partition_munmap(map);
        return;
    }

    esp_loader_connect_args_t cargs = ESP_LOADER_CONNECT_DEFAULT();
    if (esp_loader_connect(&loader, &cargs) != ESP_LOADER_SUCCESS) {
        // A radio that will not enter download mode is not a reason to keep the
        // whole bridge down — it may well be running fine.
        ESP_LOGW(TAG, "radio did not enter download mode — keeping whatever it runs");
        esp_loader_deinit(&loader);
        esp_partition_munmap(map);
        release_straps();
        return;
    }
    s_info.present = true;
    s_info.target  = (int)esp_loader_get_target(&loader);
    ESP_LOGI(TAG, "radio in download mode, target=%d", s_info.target);

    // Faster transfer once the ROM loader is up; not fatal if it refuses.
    if (esp_loader_change_transmission_rate(&loader, RADIO_FLASH_XFER_BAUD) == ESP_LOADER_SUCCESS) {
        ESP_LOGI(TAG, "transfer rate raised to %d", RADIO_FLASH_XFER_BAUD);
    }

    // Informational only — what the radio *claims* to be.  Never the basis for
    // the write decision: an interrupted write leaves a perfectly valid
    // descriptor behind (it sits at 131 KB, the image is 719 KB), so a device
    // that lost power mid-flash would look "in sync" forever and never heal.
    // Bench-proven: a reset during the write left the H2 boot-looping with a
    // valid app_desc and 0xFF from 382 KB onwards.
    esp_app_desc_t have;
    if (esp_loader_flash_read(&loader, (uint8_t *)&have, APP_DESC_ADDR, sizeof(have))
            == ESP_LOADER_SUCCESS && desc_valid(&have)) {
        ESP_LOGI(TAG, "radio currently runs: %s %s", have.project_name, have.version);
        strlcpy(s_info.running, have.version,      sizeof(s_info.running));
        strlcpy(s_info.project, have.project_name, sizeof(s_info.project));
    } else {
        ESP_LOGW(TAG, "radio has no readable app descriptor (blank or foreign image)");
    }

    // THE decision: does the radio's APPLICATION match what we staged?  MD5
    // catches everything the descriptor cannot — a half-written image, silent
    // corruption, a foreign firmware carrying the same version string.
    //
    // Only from APP_IMAGE_ADDR on, NOT from 0: the first 0x20000 of the image
    // also cover the radio's nvs, otadata and phy_init partitions, and those are
    // rewritten as soon as the firmware runs (RF calibration lands in phy_init
    // on the very first boot).  Hashing from 0 therefore never matches again
    // after the first start, and the board would re-flash its radio on EVERY
    // boot — 24 s of startup delay plus needless flash wear.  Bench-observed.
    // The bootloader and partition table below it are static and are rewritten
    // along with the app anyway whenever this check fires.
    const uint32_t app_len = img_size - APP_IMAGE_ADDR;
    s_info.app_bytes = app_len;
    char want_md5[33];
    radio_md5_hex(img + APP_IMAGE_ADDR, app_len, want_md5);

    const esp_loader_error_t verr = esp_loader_flash_verify_known_md5(
        &loader, APP_IMAGE_ADDR, app_len, (const uint8_t *)want_md5);
    const bool need_write = (verr != ESP_LOADER_SUCCESS);

    if (!need_write) {
        ESP_LOGI(TAG, "radio already in sync (app md5 match over %" PRIu32 " B) — not rewriting",
                 app_len);
    } else {
        if (verr == ESP_LOADER_ERROR_INVALID_MD5) {
            ESP_LOGW(TAG, "radio flash does not match the staged image");
        } else {
            ESP_LOGW(TAG, "cannot verify radio flash (err %d) — rewriting to be safe", (int)verr);
        }
        ESP_LOGW(TAG, "flashing radio -> %s %s", want.project_name, want.version);

        esp_loader_flash_cfg_t cfg = {
            .offset      = 0,
            .image_size  = img_size,
            .block_size  = FLASH_BLOCK_SIZE,
            .skip_verify = false,
        };

        esp_loader_error_t err = esp_loader_flash_start(&loader, &cfg);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "flash_start failed (%d) — radio left as it was", (int)err);
            goto done;
        }

        uint32_t written = 0;
        while (written < img_size) {
            const uint32_t chunk = (img_size - written) < FLASH_BLOCK_SIZE
                                 ? (img_size - written) : FLASH_BLOCK_SIZE;
            err = esp_loader_flash_write(&loader, &cfg, img + written, chunk);
            if (err != ESP_LOADER_SUCCESS) {
                ESP_LOGE(TAG, "flash_write failed at %" PRIu32 " (%d)", written, (int)err);
                goto done;
            }
            written += chunk;
            if ((written % (64 * 1024)) == 0) {
                ESP_LOGI(TAG, "  %" PRIu32 "/%" PRIu32 " KB", written / 1024, img_size / 1024);
            }
        }

        // finish() is what verifies the MD5 — a write loop without it proves
        // nothing about what landed in the radio's flash.
        err = esp_loader_flash_finish(&loader, &cfg);
        if (err != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "flash verify FAILED (%d) — radio image is suspect", (int)err);
            goto done;
        }
        ESP_LOGI(TAG, "radio flashed + verified: %s %s", want.project_name, want.version);
        s_info.rewritten = true;
        strlcpy(s_info.running, want.version,      sizeof(s_info.running));
        strlcpy(s_info.project, want.project_name, sizeof(s_info.project));
    }

done:
    // Out of download mode and into the application, then hand the UART and the
    // straps over to the bridge.
    esp_loader_reset_target(&loader);
    esp_loader_deinit(&loader);
    if (uart_is_driver_installed(UART_TCM_PORT)) {
        uart_driver_delete(UART_TCM_PORT);
    }
    esp_partition_munmap(map);
    release_straps();
}
