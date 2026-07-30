// SPDX-License-Identifier: GPL-2.0-or-later

#include "serialcfg.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "serialcfg";

#define NS  "serialcfg"   // own namespace — NOT the shared "cdc2net" ns
#define VER 1

// One NVS blob per slot, key "scfg0".."scfg7" (≤15 chars).  The device-key
// string lives inside the value, so distinct device keys can never alias.
typedef struct __attribute__((packed)) {
    uint8_t  ver;       // = VER
    uint8_t  used;      // 1 = occupied
    uint32_t baud;
    uint8_t  bits;
    uint8_t  parity;
    uint8_t  stop;
    char     key[SERIALCFG_KEY_LEN];
} entry_t;

static void slot_name(int i, char *out, size_t cap) {
    snprintf(out, cap, "scfg%d", i);
}

// Load slot i.  Returns true only for a current-version, occupied entry.
static bool load_slot(nvs_handle_t h, int i, entry_t *e) {
    char k[8]; slot_name(i, k, sizeof(k));
    size_t sz = sizeof(*e);
    if (nvs_get_blob(h, k, e, &sz) != ESP_OK) return false;
    if (sz != sizeof(*e) || e->ver != VER || !e->used) return false;
    e->key[SERIALCFG_KEY_LEN - 1] = '\0';
    return true;
}

static esp_err_t store_slot(nvs_handle_t h, int i, const entry_t *e) {
    char k[8]; slot_name(i, k, sizeof(k));
    return nvs_set_blob(h, k, e, sizeof(*e));
}

bool serialcfg_lookup(const char *key, serialcfg_lc_t *out) {
    if (!key || !key[0] || !out) return false;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool found = false;
    for (int i = 0; i < SERIALCFG_MAX; i++) {
        entry_t e;
        if (load_slot(h, i, &e) && strcmp(e.key, key) == 0) {
            out->baud   = e.baud;
            out->bits   = e.bits;
            out->parity = e.parity;
            out->stop   = e.stop;
            found = true;
            break;
        }
    }
    nvs_close(h);
    return found;
}

esp_err_t serialcfg_upsert(const char *key, const serialcfg_lc_t *lc) {
    if (!key || !key[0] || !lc) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    int match = -1, free_slot = -1;
    for (int i = 0; i < SERIALCFG_MAX; i++) {
        entry_t e;
        if (load_slot(h, i, &e)) {
            if (strcmp(e.key, key) == 0) { match = i; break; }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    int slot = (match >= 0) ? match : (free_slot >= 0 ? free_slot : 0); // evict 0

    entry_t e = {
        .ver = VER, .used = 1,
        .baud = lc->baud, .bits = lc->bits, .parity = lc->parity, .stop = lc->stop,
    };
    snprintf(e.key, sizeof(e.key), "%s", key);

    err = store_slot(h, slot, &e);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK)
        ESP_LOGI(TAG, "saved slot %d key='%s' %u %u%c%u",
                 slot, key, (unsigned)lc->baud, lc->bits,
                 "NOEMS"[lc->parity <= 4 ? lc->parity : 0], lc->stop == 0 ? 1 : 2);
    else
        ESP_LOGE(TAG, "upsert key='%s' failed: %s", key, esp_err_to_name(err));
    return err;
}

esp_err_t serialcfg_delete(const char *key) {
    if (!key || !key[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < SERIALCFG_MAX; i++) {
        entry_t e;
        if (load_slot(h, i, &e) && strcmp(e.key, key) == 0) {
            e.used = 0;
            err = store_slot(h, i, &e);
            if (err == ESP_OK) err = nvs_commit(h);
            break;
        }
    }
    nvs_close(h);
    return err;
}

int serialcfg_list(serialcfg_item_t *out, int cap) {
    if (!out || cap <= 0) return 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    int n = 0;
    for (int i = 0; i < SERIALCFG_MAX && n < cap; i++) {
        entry_t e;
        if (load_slot(h, i, &e)) {
            snprintf(out[n].key, sizeof(out[n].key), "%s", e.key);
            out[n].lc.baud   = e.baud;
            out[n].lc.bits   = e.bits;
            out[n].lc.parity = e.parity;
            out[n].lc.stop   = e.stop;
            n++;
        }
    }
    nvs_close(h);
    return n;
}
