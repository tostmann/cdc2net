// SPDX-License-Identifier: GPL-2.0-or-later

#include "sink_tcp.h"
#include "bridge.h"
#include "net.h"
#include "version.h"        // FW_VERSION_STRING (RFC2217 SIGNATURE reply)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "sink-tcp";

// ───── Telnet / RFC2217 (COM-PORT-OPTION, opt 44 / 0x2C) ─────────────────
//
// One raw-TCP port serves BOTH plain culfw/FHEM clients (byte-transparent,
// never any 0xFF) AND RFC2217 control clients (pyserial rfc2217://…, socat,
// ser2net).  A telnet client leads with IAC (0xFF) — its very first byte is
// the start of a negotiation burst (pyserial sends IAC DO ECHO, IAC WILL SGA,
// … IAC WILL COM-PORT-OPTION).  A culfw/FHEM client leads with an ASCII byte
// and never emits 0xFF.  So the FIRST byte decides the class:
//   first byte != 0xFF  → lock RAW forever (zero-overhead, byte-identical)
//   first byte == 0xFF  → telnet framing (strip IAC inbound, double 0xFF out)
// Known edge: a raw *binary* client (e.g. Modbus-RTU) that legitimately leads
// with 0xFF would be misread as telnet — inherent to sharing one port; such a
// client gets a dedicated raw port later.  No culfw stick speaks binary.
//
// RFC2217 ownership = SINGLE CONTROLLING CONNECTION: the first client to
// negotiate COM-PORT-OPTION becomes S.ctrl_client and is the only one whose
// SET-BAUDRATE/DATASIZE/PARITY/STOPSIZE is applied to the wire.  Other telnet
// clients get clean framing + answer-with-current (RFC2217-legal) but cannot
// change line params under a live session.  On controller disconnect the wire
// reverts to the device's NVS/global default.

#define TC_IAC   0xFF   // Interpret As Command
#define TC_DONT  0xFE
#define TC_DO    0xFD
#define TC_WONT  0xFC
#define TC_WILL  0xFB
#define TC_SB    0xFA   // Subnegotiation Begin
#define TC_SE    0xF0   // Subnegotiation End

#define TOPT_BINARY   0x00
#define TOPT_ECHO     0x01
#define TOPT_SGA      0x03
#define TOPT_COMPORT  0x2C

// RFC2217 COM-PORT-OPTION client suboption commands; server responses = +100.
#define CPO_SIGNATURE        0
#define CPO_SET_BAUDRATE     1
#define CPO_SET_DATASIZE     2
#define CPO_SET_PARITY       3
#define CPO_SET_STOPSIZE     4
#define CPO_SET_CONTROL      5
#define CPO_LINESTATE_MASK   10
#define CPO_MODEMSTATE_MASK  11
#define CPO_PURGE_DATA       12
#define CPO_SERVER_BASE      100

// Per-client telnet parse state.
typedef enum {
    TS_SNIFF = 0,   // initial: decide RAW vs telnet on the first byte
    TS_RAW,         // locked raw passthrough (culfw/FHEM) — byte-identical
    TS_DATA,        // telnet: ordinary data byte
    TS_IAC,         // telnet: saw IAC, expect command byte
    TS_NEG,         // telnet: saw IAC {WILL|DO|WONT|DONT}, expect option byte
    TS_SB,          // telnet: collecting subnegotiation payload
    TS_SBIAC,       // telnet: saw IAC inside subnegotiation
} tstate_t;

typedef struct {
    int          sock;
    uint32_t     ip;
    uint16_t     port;
    bool         active;
    TaskHandle_t task;
    // telnet / RFC2217 state (per connection)
    uint8_t      tstate;         // tstate_t
    bool         telnet;         // committed to telnet framing (read by on_rx)
    bool         is_controller;  // owns the wire line params (RFC2217)
    uint8_t      neg_cmd;        // pending WILL/DO/WONT/DONT in TS_NEG
    uint8_t      sb[40];         // subnegotiation collect buffer (opt+cmd+values)
    uint8_t      sb_len;
} tcp_client_t;

static struct {
    sink_t          self;
    uint16_t        port;
    int             listen_sock;
    SemaphoreHandle_t mtx;
    tcp_client_t    clients[SINK_TCP_MAX_CLIENTS];
    tcp_client_t   *ctrl_client;      // RFC2217 controlling connection (NULL=none)
    sink_tcp_stats_t stats;
} S;

// ───── Connection-Slots ─────────────────────────────────────────────────

static tcp_client_t *find_free_slot(void)
{
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) {
        if (!S.clients[i].active) return &S.clients[i];
    }
    return NULL;
}

static void close_client(tcp_client_t *c)
{
    if (!c->active) return;
    if (c->sock >= 0) {
        shutdown(c->sock, SHUT_RDWR);
        close(c->sock);
        c->sock = -1;
    }
    c->active = false;
    S.stats.total_disconnects++;
    S.stats.active_clients--;
}

// ───── Telnet / RFC2217 codec (per client, runs in client_task) ─────────

// Forward a run of *data* bytes to the source (the same path as the raw fast
// path).  rx_bytes_from_clients counts payload bytes (== wire bytes for raw
// clients, so the tcp.rx==usb.tx invariant holds; telnet framing bytes are
// excluded, which is the meaningful count).
static void to_source(const uint8_t *data, size_t len)
{
    if (!len) return;
    S.stats.rx_bytes_from_clients += (uint32_t)len;
    esp_err_t err = bridge_tx_to_source(&S.self, data, len);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != BRIDGE_ERR_TX_LOCKED)
        ESP_LOGW(TAG, "bridge_tx_to_source failed: 0x%x", err);
}

// Best-effort raw send of a few telnet control bytes (negotiation/subneg
// reply).  Called from client_task without S.mtx; lwIP serialises concurrent
// send() on the same socket and each send() is atomic, so the IAC sequence is
// never interleaved with on_rx data bytes.  The fd is only closed by this same
// task's cleanup (after the recv loop), so no reuse race here.
static void tn_send(tcp_client_t *c, const uint8_t *b, size_t n)
{
    // Telnet control frames (negotiation 3 B; COM-PORT subneg replies <=72 B)
    // MUST go out WHOLE: a short write that cuts an "IAC SB … IAC SE" before the
    // SE hangs a compliant client's subnegotiation parser (it waits forever for
    // an SE that never arrives).  Complete the write — tiny frames almost always
    // fit the send buffer in one shot; the loop is a stall safety-net.  Runs in
    // client_task with NO lock held, so the brief vTaskDelay can't stall the
    // fanout or hold a mutex.
    if (c->sock < 0) return;
    size_t off = 0;
    int stalls = 0;
    while (off < n) {
        ssize_t w = send(c->sock, b + off, n - off, MSG_DONTWAIT);
        if (w > 0) { off += (size_t)w; stalls = 0; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > 50) {                 // ~100 ms: peer not draining → drop
                ESP_LOGW(TAG, "tn_send sock=%d stalled — closing", c->sock);
                shutdown(c->sock, SHUT_RDWR);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        shutdown(c->sock, SHUT_RDWR);            // hard error / peer closed
        return;
    }
}

// First client to negotiate COM-PORT-OPTION owns the wire line params.
static void comport_acquire(tcp_client_t *c)
{
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    if (S.ctrl_client == NULL) {
        S.ctrl_client    = c;
        c->is_controller = true;
        ESP_LOGI(TAG, "RFC2217 controller acquired by sock=%d", c->sock);
    }
    xSemaphoreGive(S.mtx);
}

// Commit this client to telnet framing.  Set under S.mtx so on_rx (which reads
// c->telnet to decide outbound 0xFF-doubling) sees a coherent value.
static void tn_commit(tcp_client_t *c)
{
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    c->telnet = true;
    xSemaphoreGive(S.mtx);
}

// Respond to an inbound IAC {WILL|DO|WONT|DONT} <option>.  Loop-safe: each
// inbound request yields at most one reply; compliant clients don't re-request.
static void tn_negotiate(tcp_client_t *c, uint8_t cmd, uint8_t opt)
{
    uint8_t r[3] = { TC_IAC, 0, opt };
    switch (opt) {
    case TOPT_COMPORT:
    case TOPT_SGA:
    case TOPT_BINARY:
        // Agree both directions (COM-PORT we must; SGA/BINARY are harmless and
        // keep the link 8-bit clean).
        if      (cmd == TC_WILL) r[1] = TC_DO;
        else if (cmd == TC_DO)   r[1] = TC_WILL;
        else return;                              // WONT/DONT: swallow
        tn_send(c, r, 3);
        if (opt == TOPT_COMPORT) comport_acquire(c);
        return;
    case TOPT_ECHO:
        // We never echo.
        if      (cmd == TC_DO)   r[1] = TC_WONT;  // asked us to echo → no
        else if (cmd == TC_WILL) r[1] = TC_DONT;  // offered to echo → no thanks
        else return;
        tn_send(c, r, 3);
        return;
    default:
        // Refuse every other option.
        if      (cmd == TC_WILL) r[1] = TC_DONT;
        else if (cmd == TC_DO)   r[1] = TC_WONT;
        else return;
        tn_send(c, r, 3);
        return;
    }
}

// RFC2217 <-> CDC line-coding conversions (parity 1..5<->0..4; stop 1/2/3 RFC
// where 3==1.5 <-> CDC 0/1/2 where 1==1.5).
static inline uint8_t rfc_to_cdc_parity(uint8_t r) { return (r >= 1 && r <= 5) ? (uint8_t)(r - 1) : 0; }
static inline uint8_t cdc_to_rfc_parity(uint8_t c) { return (uint8_t)(c + 1); }
static inline uint8_t rfc_to_cdc_stop(uint8_t r)   { return r == 2 ? 2 : (r == 3 ? 1 : 0); }
static inline uint8_t cdc_to_rfc_stop(uint8_t c)   { return c == 2 ? 2 : (c == 1 ? 3 : 1); }

// Send an RFC2217 COM-PORT-OPTION server reply: IAC SB COM-PORT resp val.. IAC SE.
static void cpo_reply(tcp_client_t *c, uint8_t resp_cmd, const uint8_t *val, int vlen)
{
    uint8_t r[72];
    int k = 0;
    r[k++] = TC_IAC; r[k++] = TC_SB; r[k++] = TOPT_COMPORT; r[k++] = resp_cmd;
    for (int j = 0; j < vlen && k < (int)sizeof(r) - 3; j++) {
        r[k++] = val[j];
        if (val[j] == TC_IAC) r[k++] = TC_IAC;        // escape 0xFF in payload
    }
    r[k++] = TC_IAC; r[k++] = TC_SE;
    tn_send(c, r, (size_t)k);
}

// Dispatch a completed COM-PORT-OPTION subnegotiation (sb[] = opt cmd values…).
// Only the controller may apply; others get answer-with-current (RFC2217-legal).
static void tn_subneg(tcp_client_t *c)
{
    if (c->sb_len < 1 || c->sb[0] != TOPT_COMPORT) return;   // only COM-PORT
    uint8_t cmd = (c->sb_len >= 2) ? c->sb[1] : 0xFF;
    const uint8_t *val = &c->sb[2];
    int vlen = (c->sb_len >= 2) ? (c->sb_len - 2) : 0;

    // Snapshot controller ownership coherently — on_source_down() clears
    // is_controller from another task.  Take+release S.mtx for the read ONLY: it
    // must NOT be held across the bridge line-coding calls below (those take the
    // source tx_mtx and must never nest under the sink mutex — load-bearing).
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    bool ctrl = c->is_controller;
    xSemaphoreGive(S.mtx);

    uint32_t baud = 0; uint8_t bits = 0, parity = 0, stop = 0;
    bridge_get_line_coding(&baud, &bits, &parity, &stop);    // current (CDC form)

    switch (cmd) {
    case CPO_SIGNATURE: {
        const char *sig = "CDC2NET " FW_VERSION_STRING;
        cpo_reply(c, CPO_SERVER_BASE + CPO_SIGNATURE, (const uint8_t *)sig, (int)strlen(sig));
        return;
    }
    case CPO_SET_BAUDRATE: {
        if (vlen >= 4) {
            uint32_t req = ((uint32_t)val[0] << 24) | ((uint32_t)val[1] << 16) |
                           ((uint32_t)val[2] <<  8) |  (uint32_t)val[3];
            if (req != 0 && ctrl &&
                bridge_apply_line_coding(req, bits, parity, stop) == ESP_OK)
                baud = req;
        }
        uint8_t r[4] = { (uint8_t)(baud >> 24), (uint8_t)(baud >> 16),
                         (uint8_t)(baud >>  8), (uint8_t)baud };
        cpo_reply(c, CPO_SERVER_BASE + CPO_SET_BAUDRATE, r, 4);
        return;
    }
    case CPO_SET_DATASIZE: {
        if (vlen >= 1 && val[0] != 0 && ctrl &&
            bridge_apply_line_coding(baud, val[0], parity, stop) == ESP_OK)
            bits = val[0];
        uint8_t r = bits;
        cpo_reply(c, CPO_SERVER_BASE + CPO_SET_DATASIZE, &r, 1);
        return;
    }
    case CPO_SET_PARITY: {
        if (vlen >= 1 && val[0] != 0 && ctrl) {
            uint8_t cp = rfc_to_cdc_parity(val[0]);
            if (bridge_apply_line_coding(baud, bits, cp, stop) == ESP_OK) parity = cp;
        }
        uint8_t r = cdc_to_rfc_parity(parity);
        cpo_reply(c, CPO_SERVER_BASE + CPO_SET_PARITY, &r, 1);
        return;
    }
    case CPO_SET_STOPSIZE: {
        if (vlen >= 1 && val[0] != 0 && ctrl) {
            uint8_t cs = rfc_to_cdc_stop(val[0]);
            if (bridge_apply_line_coding(baud, bits, parity, cs) == ESP_OK) stop = cs;
        }
        uint8_t r = cdc_to_rfc_stop(stop);
        cpo_reply(c, CPO_SERVER_BASE + CPO_SET_STOPSIZE, &r, 1);
        return;
    }
    case CPO_SET_CONTROL: {
        // Stub (v0.2): accept, no wire effect — driving DTR/RTS could reset
        // AVR-bootloader sticks (see CLAUDE.md).  Echo a sensible state.
        uint8_t req = (vlen >= 1) ? val[0] : 0;
        uint8_t r;
        if      (req <= 3)  r = 1;    // flow (query/set, in+out) → NO-FLOW
        else if (req <= 6)  r = 6;    // BREAK (query/set)        → BREAK-OFF
        else if (req <= 9)  r = 8;    // DTR  (query/set)         → DTR-ON
        else if (req <= 12) r = 11;   // RTS  (query/set)         → RTS-ON
        else                r = 1;    // inbound-flow variants    → NO-FLOW
        cpo_reply(c, CPO_SERVER_BASE + CPO_SET_CONTROL, &r, 1);
        return;
    }
    case CPO_PURGE_DATA: {
        uint8_t r = (vlen >= 1) ? val[0] : 0;     // echo (buffers already flat)
        cpo_reply(c, CPO_SERVER_BASE + CPO_PURGE_DATA, &r, 1);
        return;
    }
    case CPO_LINESTATE_MASK:
    case CPO_MODEMSTATE_MASK: {
        uint8_t r = 0;                            // we send no async notifies (v0.2)
        cpo_reply(c, CPO_SERVER_BASE + cmd, &r, 1);
        return;
    }
    default:
        return;                                   // unknown suboption — swallow
    }
}

// Per-byte telnet FSM over one recv chunk.  Pure data bytes are coalesced into
// `out` and flushed to the source; telnet framing is stripped; subnegotiations
// are dispatched.  Once locked RAW it bulk-forwards the remaining slice (so the
// culfw/FHEM path stays byte-identical with a single bridge_tx_to_source call).
static void process_rx(tcp_client_t *c, const uint8_t *buf, int n)
{
    uint8_t out[256];
    int oi = 0;
    int i = 0;

    while (i < n) {
        if (c->tstate == TS_RAW) {                // raw fast path
            if (oi) { to_source(out, oi); oi = 0; }
            to_source(buf + i, (size_t)(n - i));
            return;
        }

        uint8_t b = buf[i++];

        switch (c->tstate) {
        case TS_SNIFF:
            if (b != TC_IAC) {
                c->tstate = TS_RAW;               // ASCII/culfw — lock RAW
                i--;                              // reprocess via the RAW fast path
            } else {
                tn_commit(c);                     // leads with IAC → telnet
                c->tstate = TS_IAC;
            }
            break;

        case TS_DATA:
            if (b == TC_IAC) {
                if (oi) { to_source(out, oi); oi = 0; }
                c->tstate = TS_IAC;
            } else {
                out[oi++] = b;
                if (oi == (int)sizeof(out)) { to_source(out, oi); oi = 0; }
            }
            break;

        case TS_IAC:
            if (b == TC_IAC) {                     // IAC IAC → literal 0xFF data
                out[oi++] = 0xFF;
                if (oi == (int)sizeof(out)) { to_source(out, oi); oi = 0; }
                c->tstate = TS_DATA;
            } else if (b == TC_SB) {
                c->sb_len = 0;
                c->tstate = TS_SB;
            } else if (b == TC_WILL || b == TC_WONT || b == TC_DO || b == TC_DONT) {
                c->neg_cmd = b;
                c->tstate = TS_NEG;
            } else {
                c->tstate = TS_DATA;               // other 2-byte command: ignore
            }
            break;

        case TS_NEG:
            tn_negotiate(c, c->neg_cmd, b);
            c->tstate = TS_DATA;
            break;

        case TS_SB:
            if (b == TC_IAC) {
                c->tstate = TS_SBIAC;
            } else if (c->sb_len < sizeof(c->sb)) {
                c->sb[c->sb_len++] = b;            // overflow: drop excess, keep collecting
            }
            break;

        case TS_SBIAC:
            if (b == TC_IAC) {                     // escaped 0xFF inside subneg
                if (c->sb_len < sizeof(c->sb)) c->sb[c->sb_len++] = 0xFF;
                c->tstate = TS_SB;
            } else if (b == TC_SE) {               // end of subnegotiation
                tn_subneg(c);
                c->tstate = TS_DATA;
            } else {                               // malformed: end + reprocess byte
                tn_subneg(c);
                c->tstate = TS_DATA;
                i--;
            }
            break;

        default:
            c->tstate = TS_DATA;
            break;
        }
    }
    if (oi) to_source(out, oi);
}

// ───── client_task (per Client; reads bytes → bridge.tx) ────────────────

static void client_task(void *arg)
{
    tcp_client_t *c = (tcp_client_t *)arg;
    uint8_t buf[256];
    ESP_LOGI(TAG, "client task started for sock=%d (%u.%u.%u.%u:%u)",
             c->sock, (c->ip      ) & 0xff, (c->ip >>  8) & 0xff,
                      (c->ip >> 16) & 0xff, (c->ip >> 24) & 0xff, c->port);

    while (c->active && c->sock >= 0) {
        int n = recv(c->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        // Telnet/RFC2217 FSM (decides RAW vs telnet on the first byte, strips
        // IAC framing, dispatches COM-PORT-OPTION subnegotiations).  Drops if
        // source not ready / TX-lock held by another sink (handled in to_source).
        process_rx(c, buf, n);
    }

    // Cleanup.  If this client owned the RFC2217 wire, release ownership under
    // S.mtx, then revert the line coding AFTER dropping S.mtx — revert takes the
    // source tx-lock and must never be called with the sink mutex held.
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    bool was_ctrl = (S.ctrl_client == c);
    if (was_ctrl) { S.ctrl_client = NULL; c->is_controller = false; }
    close_client(c);
    xSemaphoreGive(S.mtx);
    if (was_ctrl) {
        ESP_LOGI(TAG, "RFC2217 controller released (disconnect) → revert line coding");
        bridge_revert_line_coding();
    }
    ESP_LOGI(TAG, "client task exit");
    c->task = NULL;
    vTaskDelete(NULL);
}

// ───── accept_task ──────────────────────────────────────────────────────

static void accept_task(void *arg)
{
    (void)arg;

    // Wait until WiFi is up so bind() won't fail with EAFNOSUPPORT etc.
    // Either an STA connection OR the captive SoftAP counts — in AP-only
    // mode the listener still binds on 0.0.0.0 (reachable via 192.168.4.1).
    while (!net_is_connected() && !net_is_ap_mode()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(srv, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(S.port),
    };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(:%u) failed: errno=%d", S.port, errno);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    if (listen(srv, 2) < 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(srv);
        vTaskDelete(NULL);
        return;
    }
    S.listen_sock = srv;
    ESP_LOGI(TAG, "raw-TCP listener on :%u", S.port);

    while (1) {
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);
        int cs = accept(srv, (struct sockaddr *)&cli, &cli_len);
        if (cs < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "accept failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Gate on source readiness: with no USB device present/ready, don't
        // hand the client a usable session — close immediately so downstream
        // (FHEM) keeps retrying and only sticks (and re-initialises: re-reads
        // the attached stick's BaseID/version) once a device is actually
        // ready.  Without this the listener accepts into an empty bridge and
        // FHEM re-establishes a STALE session against no device — so a
        // hot-swap to a different stick would never be picked up.  Combined
        // with on_source_down (drop on disconnect) this makes the invariant
        // "a TCP client is connected  ⟺  a live device is behind the bridge".
        if (!source_ready(bridge_get_source())) {
            close(cs);
            static uint32_t s_rej;
            if ((s_rej++ & 7u) == 0u)
                ESP_LOGI(TAG, "accept rejected: no source ready "
                              "(downstream will retry) [#%u]", (unsigned)s_rej);
            continue;
        }

        // TCP_NODELAY + SO_KEEPALIVE for the new client too.
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int ka = 1;
        setsockopt(cs, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));

        // Non-blocking RX would be nicer, but blocking + recv-loop is
        // simpler and safe with a per-client task.

        S.stats.total_accepts++;

        xSemaphoreTake(S.mtx, portMAX_DELAY);
        tcp_client_t *slot = find_free_slot();
        if (!slot) {
            xSemaphoreGive(S.mtx);
            ESP_LOGW(TAG, "no free client slot — closing %d.%d.%d.%d:%u",
                     (int)((cli.sin_addr.s_addr      ) & 0xff),
                     (int)((cli.sin_addr.s_addr >>  8) & 0xff),
                     (int)((cli.sin_addr.s_addr >> 16) & 0xff),
                     (int)((cli.sin_addr.s_addr >> 24) & 0xff),
                     ntohs(cli.sin_port));
            close(cs);
            continue;
        }
        slot->sock          = cs;
        slot->ip            = cli.sin_addr.s_addr;
        slot->port          = ntohs(cli.sin_port);
        // Reset per-connection telnet state on (re)use of this slot.
        slot->tstate        = TS_SNIFF;
        slot->telnet        = false;
        slot->is_controller = false;
        slot->neg_cmd       = 0;
        slot->sb_len        = 0;
        slot->active        = true;
        S.stats.active_clients++;
        xSemaphoreGive(S.mtx);

        char name[32];
        snprintf(name, sizeof(name), "tcp_cli_%d", cs);
        xTaskCreate(client_task, name, 4096, slot, 4, &slot->task);
    }
}

// ───── sink_t-Hooks ─────────────────────────────────────────────────────

// Send one fanout of source bytes to a single client.  Raw clients get the
// zero-copy path (byte-identical).  Telnet clients get every 0xFF doubled (IAC
// escape), chunked through a shared static buffer (we hold S.mtx → exclusive,
// so the static is safe and off the small USB-driver-task stack).  Best-effort,
// same drop-on-EAGAIN / shutdown-on-hard-error semantics as the raw path.
static void client_send(tcp_client_t *c, const uint8_t *data, size_t len)
{
    if (!c->telnet) {
        ssize_t w = send(c->sock, data, len, MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) S.stats.tx_dropped_eagain++;
            else { ESP_LOGW(TAG, "send to sock=%d failed errno=%d — closing", c->sock, errno);
                   shutdown(c->sock, SHUT_RDWR); }
        } else {
            S.stats.tx_bytes_to_clients += (uint32_t)w;
        }
        return;
    }

    // Best-effort, like the raw path: this runs in the fanout (driver task,
    // under S.mtx) and MUST NOT block, so a short send() drops the rest of this
    // fanout for this client.  A partial send could in theory split a doubled
    // 0xFF (leaving a lone IAC the client misreads) — but only under send-buffer
    // pressure AND only for sources that emit 0xFF; culfw data is ASCII so no
    // escape pairs ever form.  Binary VCP sources at line-rate are the edge.
    static uint8_t esc[256];            // S.mtx-guarded (on_rx holds it)
    size_t ei = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (ei + 2 > sizeof(esc)) {
            ssize_t w = send(c->sock, esc, ei, MSG_DONTWAIT);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) S.stats.tx_dropped_eagain++;
                else { ESP_LOGW(TAG, "send(telnet) sock=%d errno=%d — closing", c->sock, errno);
                       shutdown(c->sock, SHUT_RDWR); }
                return;                  // drop the rest of this fanout for this client
            }
            S.stats.tx_bytes_to_clients += (uint32_t)w;
            ei = 0;
        }
        esc[ei++] = b;
        if (b == TC_IAC) esc[ei++] = TC_IAC;
    }
    if (ei) {
        ssize_t w = send(c->sock, esc, ei, MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) S.stats.tx_dropped_eagain++;
            else { ESP_LOGW(TAG, "send(telnet) sock=%d errno=%d — closing", c->sock, errno);
                   shutdown(c->sock, SHUT_RDWR); }
        } else {
            S.stats.tx_bytes_to_clients += (uint32_t)w;
        }
    }
}

static void on_rx(sink_t *s, const uint8_t *data, size_t len)
{
    (void)s;
    if (!data || !len) return;

    // Mutex hält für den ganzen Fanout-Loop — schützt gegen fd-
    // Wiederverwendung zwischen Snapshot und send (close in client_task
    // gibt fd frei, accept_task kann sofort denselben fd für eine andere
    // Verbindung kassieren) und gegen torn c->telnet-Reads.  send() ist
    // MSG_DONTWAIT, also non-blocking; bei SINK_TCP_MAX_CLIENTS=4 ist die
    // Hold-Time vernachlässigbar.
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) {
        tcp_client_t *c = &S.clients[i];
        if (!c->active || c->sock < 0) continue;
        client_send(c, data, len);
    }
    xSemaphoreGive(S.mtx);
}

// Source (USB-Stick) ist weg → alle Client-Verbindungen droppen, damit
// Downstream (FHEM/nc) den Disconnect sieht, neu connectet und gegen den
// als nächstes angesteckten Stick neu initialisiert.  Nur `shutdown()` —
// der zugehörige client_task wacht aus recv() auf, bricht seine Schleife
// und macht close_client() im Cleanup-Pfad (gleicher Handoff wie der
// Send-Fehler-Pfad in on_rx; vermeidet Double-Close / fd-Race).
static void on_source_down(sink_t *s)
{
    (void)s;
    int dropped = 0;
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    // Device is gone — drop RFC2217 ownership.  No line-coding revert needed:
    // the next device's stick_task re-applies its own NVS/default coding on
    // open (and S.cdc is NULL now, so a revert would be a no-op anyway).
    S.ctrl_client = NULL;
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) {
        tcp_client_t *c = &S.clients[i];
        c->is_controller = false;
        if (c->active && c->sock >= 0) {
            shutdown(c->sock, SHUT_RDWR);
            dropped++;
        }
    }
    xSemaphoreGive(S.mtx);
    if (dropped)
        ESP_LOGI(TAG, "source down → dropping %d client(s) for re-init", dropped);
}

static const char *describe(sink_t *s)
{
    (void)s;
    static char buf[48];
    snprintf(buf, sizeof(buf), "raw-TCP :%u (%d/%d clients)",
             S.port, S.stats.active_clients, SINK_TCP_MAX_CLIENTS);
    return buf;
}

static esp_err_t op_start(sink_t *s)
{
    (void)s;
    xTaskCreate(accept_task, "tcp_accept", 4096, NULL, 4, NULL);
    return ESP_OK;
}

static const struct sink_ops s_ops = {
    .on_source_rx   = on_rx,
    .start          = op_start,
    .stop           = NULL,
    .on_source_down = on_source_down,
    .describe       = describe,
};

sink_t *sink_tcp_init(uint16_t port)
{
    memset(&S, 0, sizeof(S));
    S.port = port ? port : SINK_TCP_DEFAULT_PORT;
    S.listen_sock = -1;
    S.mtx = xSemaphoreCreateMutex();
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) S.clients[i].sock = -1;
    S.self.ops = &s_ops;
    return &S.self;
}

void sink_tcp_get_stats(sink_tcp_stats_t *out)
{
    if (!out) return;
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    *out = S.stats;
    out->port = S.port;
    xSemaphoreGive(S.mtx);
}
