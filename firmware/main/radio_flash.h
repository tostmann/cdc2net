// SPDX-License-Identifier: GPL-2.0-or-later
//
// radio_flash.h — flash the companion radio SoC from the host SoC.
//
// On boards where the radio is a second ESP chip on an internal UART (the ESP
// Thread-BR / Zigbee-GW board: ESP32-S3 host + ESP32-H2 radio), the radio's
// firmware ships in this device's own flash — the `radio_fw` partition — and is
// written to the radio over that same UART plus its reset/boot straps.  A
// factory image that includes that partition therefore flashes the whole
// device; an app-only OTA updates the host and leaves the radio alone.
//
// Espressif does the same for its own RCP (esp_rcp_update, which is likewise
// built on esp-serial-flasher); the difference is what gets written — here the
// radio runs a full ZBOSS NCP (busware esp-coordinator), not an ot_rcp, so the
// host never speaks Spinel and stays a byte-transparent bridge.
//
// Policy: the radio's running esp_app_desc is read back over the download
// protocol and compared against the embedded one.  Mismatched project name
// (e.g. a factory-fresh board still running ot_rcp) or a different version →
// write; identical → leave it alone.  So a factory flash of the host takes the
// whole board over, while a host-only OTA does not disturb a working radio.
//
// MUST run before the UART source claims the port (see main.c) — the flasher
// owns UART_TCM_PORT for the duration and releases it again.

#ifndef CDC2NET_RADIO_FLASH_H
#define CDC2NET_RADIO_FLASH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What radio_flash_sync() found and did — for the web UI, so the status page can
// show the actual radio instead of guessing from the UART profile.  Valid after
// radio_flash_sync() has run; `present` is false when the radio never answered.
typedef struct {
    bool     present;        // radio entered download mode and was readable
    int      target;         // esp_loader target id (7 = ESP32-H2)
    char     staged[32];     // version of the image in the radio_fw partition
    char     running[32];    // version the radio actually runs ("" if unknown)
    char     project[32];    // project name the radio reports ("" if unknown)
    bool     rewritten;      // true when this boot wrote a new image
    uint32_t app_bytes;      // size of the verified application region
} radio_info_t;

// Never NULL.  Before radio_flash_sync() runs, all fields are zero/empty.
const radio_info_t *radio_flash_info(void);

// Bring the companion radio in sync with the embedded firmware.  Never fatal:
// on any failure the bridge still comes up, so a board with a healthy radio
// stays usable even if the download protocol cannot be entered.
void radio_flash_sync(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_RADIO_FLASH_H
