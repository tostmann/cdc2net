// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_t / sink_t — CDC2NET-Bridge-Architektur.
//
// Datenfluss:
//
//   ┌────────────┐  rx (URB-Bytes)   ┌────────┐  fanout  ┌──────────┐
//   │ source     │ ─────────────────▶│ bridge │─────────▶│ sink[N]  │
//   │ (USB-Stick)│                   │        │          │ (TCP,    │
//   │            │◀──────────────────│        │◀─────────│  UDP,    │
//   └────────────┘  tx (raw bytes)   └────────┘  tx-req  │  WebUI…) │
//                                                        └──────────┘
//
// Eine source_t ist die Daten-Origin für Bytes — aktuell der CUL-
// Stick am USB-Host-Port (`source_usb`).  Spätere Phase-2-Hardware bringt
// optional eine UART-Pinheader-Source (`source_uart`) als zweite
// Implementierung des gleichen Interface.
//
// Eine sink_t ist ein Konsument — typischerweise ein Netzwerk-Listener
// (Raw-TCP, HB-RF-ETH-UDP, WebUI-Tail).  Sinks bekommen jeden Byte
// den die Source liefert, und können selbst Bytes Richtung Source
// schicken (z.B. TCP-Client → CDC2NET → Stick).
//
// Source und Sinks wissen nichts voneinander — die `bridge` (siehe
// bridge.h) hält die Verdrahtung, und sind die einzige Komponente die
// die N-Sink-Liste verwaltet.

#ifndef CDC2NET_SOURCE_H
#define CDC2NET_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct source source_t;
typedef struct sink   sink_t;

// ───── source-Interface (USB / UART / …) ────────────────────────────────

// Source-side "wir haben gerade RX-Bytes vom Wire bekommen"-Hook.
// Wird von der Source intern aufgerufen sobald RX-Bytes da sind; die
// Implementation liefert das an `bridge_on_source_rx()`.
typedef void (*source_rx_sink_t)(void *ctx, const uint8_t *data, size_t len);

struct source_ops {
    // Rohbytes Richtung Wire schicken.  Blocking erlaubt; Caller-Erwartung:
    // "kommt durch oder ESP_ERR_TIMEOUT".  Implementation darf intern
    // queuen / serialisieren.
    esp_err_t (*tx)(source_t *src, const uint8_t *data, size_t len);

    // True wenn die Source operational ist (= Hardware verbunden,
    // Init durch, Boot-Probe abgeschlossen).  Bevor ready==true
    // werden TX-Requests von Sinks von der Bridge verworfen.
    bool      (*ready)(source_t *src);

    // Best-effort Reset.  Bei USB-Source ohne VBUS-FET ist das ein
    // No-op (siehe Memory `pcb_basics_must_have` — Power-Cycle braucht
    // Hardware).  Mit VBUS-FET: VBUS aus → 200ms warten → VBUS an.
    esp_err_t (*reset)(source_t *src);

    // Identifier-String für Status-Anzeige (z.B. "USB:1B1F:C020 App
    // 'DualCoPro_App'").  Zeigt aktuellen Modus + Tag wenn bekannt.
    const char* (*describe)(source_t *src);

    // ── Line-coding (RFC2217 Layer B + per-device NVS default) ───────────
    // Optional (NULL = source has no settable line coding).  All values are
    // CDC form: bits 5..8, parity 0N/1O/2E/3M/4S, stop 0=1/1=1.5/2=2.
    //
    // LOCK-ORDER (load-bearing): the implementation takes ONLY its own
    // tx-lock (the same one that serialises op_tx) — it issues an EP0
    // control transfer.  Callers (the bridge passthroughs) MUST snapshot the
    // source pointer under their own mutex, RELEASE it, and only THEN call
    // these — never with a sink mutex held.  See bridge_apply_line_coding().
    //
    //   set_line_coding   — apply baud/bits/parity/stop to the wire (RFC2217)
    //   revert_line_coding— re-apply the device's NVS/global default coding
    //                       (called when the RFC2217 controller releases)
    //   get_line_coding   — read back the last-applied coding (display shadow;
    //                       FTDI/CH34x have no GET, so this is a shadow)
    esp_err_t (*set_line_coding)(source_t *src, uint32_t baud, uint8_t bits,
                                 uint8_t parity, uint8_t stop);
    void      (*revert_line_coding)(source_t *src);
    void      (*get_line_coding)(source_t *src, uint32_t *baud, uint8_t *bits,
                                 uint8_t *parity, uint8_t *stop);
};

struct source {
    const struct source_ops *ops;
    source_rx_sink_t         rx_sink;   // gesetzt von bridge_attach_source
    void                    *rx_sink_ctx;
    void                    *user;      // implementation-private
    const char              *short_id;  // "usb" / "uart" — stable, für API
};

// Convenience-Wrappers.
static inline esp_err_t source_tx(source_t *src, const uint8_t *data, size_t len) {
    return src && src->ops && src->ops->tx ? src->ops->tx(src, data, len) : ESP_ERR_INVALID_STATE;
}
static inline bool source_ready(source_t *src) {
    return src && src->ops && src->ops->ready && src->ops->ready(src);
}
static inline esp_err_t source_reset(source_t *src) {
    return src && src->ops && src->ops->reset ? src->ops->reset(src) : ESP_ERR_NOT_SUPPORTED;
}
static inline const char *source_describe(source_t *src) {
    return src && src->ops && src->ops->describe ? src->ops->describe(src) : "no-source";
}
static inline esp_err_t source_set_line_coding(source_t *src, uint32_t baud,
        uint8_t bits, uint8_t parity, uint8_t stop) {
    return src && src->ops && src->ops->set_line_coding
        ? src->ops->set_line_coding(src, baud, bits, parity, stop)
        : ESP_ERR_NOT_SUPPORTED;
}
static inline void source_revert_line_coding(source_t *src) {
    if (src && src->ops && src->ops->revert_line_coding) src->ops->revert_line_coding(src);
}
static inline void source_get_line_coding(source_t *src, uint32_t *baud,
        uint8_t *bits, uint8_t *parity, uint8_t *stop) {
    if (src && src->ops && src->ops->get_line_coding)
        src->ops->get_line_coding(src, baud, bits, parity, stop);
}

// ───── sink-Interface (TCP / UDP / WebUI / …) ───────────────────────────

struct sink_ops {
    // Source hat RX-Bytes geliefert — Sink soll sie weiterreichen
    // (z.B. an alle TCP-Clients senden, oder als Type-7-UDP-Frame
    // verpacken).  Bridge ruft das pro Sink synchron in der RX-Pump-
    // Reihenfolge.  Sink darf NICHT blockieren.
    void (*on_source_rx)(sink_t *sink, const uint8_t *data, size_t len);

    // Optional: lifecycle-Hooks.  start() z.B. um TCP-listener-Socket
    // zu öffnen, stop() zum Aufräumen.
    esp_err_t (*start)(sink_t *sink);
    esp_err_t (*stop)(sink_t *sink);

    // Optional: die Source ist weg (USB-Gerät abgesteckt).  Sink soll
    // pro-Gerät-Downstream-State fallen lassen — der TCP-Sink schließt
    // seine Client-Verbindungen, damit Downstream (z.B. FHEM) neu
    // connectet und gegen den DANN angesteckten Stick neu initialisiert.
    // Die Bridge kann einen Stick-Swap nicht von einem Reconnect
    // unterscheiden (z.B. teilen sich ESP32-C3 und -C6 die USB-Identität
    // 303A:1001) — also muss jeder Disconnect als potenzieller Wechsel
    // gelten.  Darf NULL sein (Sinks ohne pro-Gerät-State, z.B. debug).
    void (*on_source_down)(sink_t *sink);

    const char *(*describe)(sink_t *sink);
};

struct sink {
    const struct sink_ops *ops;
    void                  *user;
};

static inline void sink_on_source_rx(sink_t *s, const uint8_t *d, size_t n) {
    if (s && s->ops && s->ops->on_source_rx) s->ops->on_source_rx(s, d, n);
}
static inline esp_err_t sink_start(sink_t *s) {
    return s && s->ops && s->ops->start ? s->ops->start(s) : ESP_OK;
}
static inline esp_err_t sink_stop(sink_t *s) {
    return s && s->ops && s->ops->stop ? s->ops->stop(s) : ESP_OK;
}
static inline void sink_on_source_down(sink_t *s) {
    if (s && s->ops && s->ops->on_source_down) s->ops->on_source_down(s);
}
static inline const char *sink_describe(sink_t *s) {
    return s && s->ops && s->ops->describe ? s->ops->describe(s) : "no-sink";
}

#endif // CDC2NET_SOURCE_H
