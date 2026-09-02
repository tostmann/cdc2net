// SPDX-License-Identifier: GPL-2.0-or-later
//
// cul_dfu.h — den angesteckten AVR-Stick ueber USB neu flashen.
//
// Wozu
// ----
// Der Legacy-CUL (ATmega32U4) haengt hier am OTG-Anschluss und wird sonst als
// CDC-ACM durchgereicht. Auf Befehl springt er in seinen DFU-Bootlader und
// meldet sich neu an — dann ist er nicht mehr die serielle Quelle, sondern ein
// Ziel, das eine neue Firmware annimmt. Dieses Modul spricht diesen Bootlader.
//
// Warum das ueberhaupt geht
// -------------------------
// Das Atmel-DFU-Protokoll braucht AUSSCHLIESSLICH Steuertransfers auf
// Endpunkt 0 — keine Bulk-, keine Interrupt-Endpunkte. Das ist der einfachste
// Fall, den ein USB-Wirt bedienen kann, und der Grund, warum ein ESP32-S3 dafuer
// reicht.
//
// Ablauf einer Aktualisierung
// ---------------------------
//   1. `B01` ueber die normale Bruecke an den CUL  -> er meldet sich ab
//   2. er meldet sich als 03EB:2FF4 neu an          -> dieses Modul uebernimmt
//   3. loeschen, schreiben, Anwendung starten
//   4. er meldet sich wieder als CDC-ACM an         -> die Bruecke uebernimmt
//
// Schritt 1 muss die Gegenstelle ausloesen; ohne Befehl im Stick gibt es keinen
// Weg in den Bootlader ausser einem Handgriff am Geraet.

#ifndef CDC2NET_CUL_DFU_H
#define CDC2NET_CUL_DFU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Atmel-DFU-Bootlader des ATmega32U4.
#define CUL_DFU_VID  0x03EB
#define CUL_DFU_PID  0x2FF4

// Anwendungsbereich: alles unterhalb des 4-KB-Bootladers. Ein Abbild, das
// darueber hinausreicht, wird abgelehnt statt den Bootlader zu ueberschreiben —
// danach waere der Stick nur noch mit fremder Hardware zu retten.
#define CUL_DFU_APP_LIMIT  0x7000

typedef struct {
    bool     present;      // Bootlader haengt gerade am Bus
    bool     busy;         // eine Uebertragung laeuft
    uint16_t vid, pid;
    uint32_t written;      // Byte im letzten Lauf geschrieben
    bool     eeprom_checked; // EEPROM war vor und nach dem Lauf lesbar
    bool     eeprom_intact;  // ... und unveraendert
    char     last[96];     // letzte Meldung, auch im Fehlerfall
} cul_dfu_status_t;

// Meldet den eigenen USB-Wirt-Klienten an und startet dessen Ereignisschleife.
// Nach usb_host_install() aufzurufen, also nach dem Start der USB-Quelle.
esp_err_t cul_dfu_start(void);

// Nie NULL.
const cul_dfu_status_t *cul_dfu_status(void);

// Abbild schreiben: loeschen, programmieren, Anwendung starten.
// `img` ist ein ROHES Speicherabbild ab Adresse 0 (avr-objcopy -O binary),
// KEINE Intel-HEX-Datei. Blockiert bis zum Ende; typisch wenige Sekunden.
esp_err_t cul_dfu_flash(const uint8_t *img, size_t len);

#ifdef __cplusplus
}
#endif

#endif
