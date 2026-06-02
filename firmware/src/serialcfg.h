// SPDX-License-Identifier: GPL-2.0-or-later
//
// serialcfg.h — per-device serial line-coding store (Layer A of the per-port
// serial config feature).  Keeps a small NVS table of {baud,bits,parity,stop}
// keyed by a device-identity string: the USB iSerialNumber when present, else
// "VVVV:PPPP" (VID:PID).  CH340s usually have no serial, so they all share the
// VID:PID entry — coarse but never silently wrong.
//
// Fixed 8-slot table: NVS keys cap at 15 chars, so the device-key string can't
// be the NVS key; it lives inside each slot's blob (collision-proof, no hash).

#ifndef CDC2NET_SERIALCFG_H
#define CDC2NET_SERIALCFG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SERIALCFG_MAX          8
#define SERIALCFG_KEY_LEN      33    // 32 chars + NUL
#define SERIALCFG_DEFAULT_BAUD 115200

// Line coding for one device.  Fields map 1:1 onto cdc_acm_line_coding_t
// (usb_types_cdc.h): bits->bDataBits, parity->bParityType, stop->bCharFormat.
typedef struct {
    uint32_t baud;     // dwDTERate
    uint8_t  bits;     // 5..8
    uint8_t  parity;   // 0 N / 1 O / 2 E / 3 M / 4 S
    uint8_t  stop;     // 0=1 / 1=1.5 / 2=2
} serialcfg_lc_t;

typedef struct {
    char          key[SERIALCFG_KEY_LEN];
    serialcfg_lc_t lc;
} serialcfg_item_t;

// The global fallback used when no per-device entry matches.
static inline serialcfg_lc_t serialcfg_default(void) {
    return (serialcfg_lc_t){ .baud = SERIALCFG_DEFAULT_BAUD, .bits = 8,
                             .parity = 0, .stop = 0 };
}

// Look up `key`.  Returns true + fills *out on hit; false (out untouched) miss.
bool serialcfg_lookup(const char *key, serialcfg_lc_t *out);

// Store/replace `key`'s line coding (replace match / first free / evict slot 0).
esp_err_t serialcfg_upsert(const char *key, const serialcfg_lc_t *lc);

// Delete `key`'s entry (no-op if absent).
esp_err_t serialcfg_delete(const char *key);

// Copy up to `cap` stored entries into `out`.  Returns the count written.
int serialcfg_list(serialcfg_item_t *out, int cap);

#endif // CDC2NET_SERIALCFG_H
