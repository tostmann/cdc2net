// SPDX-License-Identifier: GPL-2.0-or-later

#include "sink_tcp.h"
#include "bridge.h"
#include "net.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "sink-tcp";

typedef struct {
    int       sock;
    uint32_t  ip;
    uint16_t  port;
    bool      active;
    TaskHandle_t task;
} tcp_client_t;

static struct {
    sink_t          self;
    uint16_t        port;
    int             listen_sock;
    SemaphoreHandle_t mtx;
    tcp_client_t    clients[SINK_TCP_MAX_CLIENTS];
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
        S.stats.rx_bytes_from_clients += (uint32_t)n;
        // Forward to source. Bridge drops if source not ready or TX-lock
        // belongs to another sink.
        esp_err_t err = bridge_tx_to_source(&S.self, buf, (size_t)n);
        if (err != ESP_OK
            && err != ESP_ERR_INVALID_STATE
            && err != BRIDGE_ERR_TX_LOCKED) {
            ESP_LOGW(TAG, "bridge_tx_to_source failed: 0x%x", err);
        }
    }

    xSemaphoreTake(S.mtx, portMAX_DELAY);
    close_client(c);
    xSemaphoreGive(S.mtx);
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
        slot->sock   = cs;
        slot->ip     = cli.sin_addr.s_addr;
        slot->port   = ntohs(cli.sin_port);
        slot->active = true;
        S.stats.active_clients++;
        xSemaphoreGive(S.mtx);

        char name[32];
        snprintf(name, sizeof(name), "tcp_cli_%d", cs);
        xTaskCreate(client_task, name, 4096, slot, 4, &slot->task);
    }
}

// ───── sink_t-Hooks ─────────────────────────────────────────────────────

static void on_rx(sink_t *s, const uint8_t *data, size_t len)
{
    (void)s;
    if (!data || !len) return;

    // Mutex hält für den ganzen Fanout-Loop — schützt gegen fd-
    // Wiederverwendung zwischen Snapshot und send (close in client_task
    // gibt fd frei, accept_task kann sofort denselben fd für eine andere
    // Verbindung kassieren).  send() ist MSG_DONTWAIT, also non-blocking;
    // bei SINK_TCP_MAX_CLIENTS=4 ist die Hold-Time vernachlässigbar.
    xSemaphoreTake(S.mtx, portMAX_DELAY);
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) {
        tcp_client_t *c = &S.clients[i];
        if (!c->active || c->sock < 0) continue;
        ssize_t w = send(c->sock, data, len, MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                S.stats.tx_dropped_eagain++;
            } else {
                ESP_LOGW(TAG, "send to sock=%d failed errno=%d — closing",
                         c->sock, errno);
                // shutdown nur — der zugehörige client_task macht close()
                // im Cleanup-Pfad.  Wir touchen den Slot hier sonst nicht.
                shutdown(c->sock, SHUT_RDWR);
            }
        } else if ((size_t)w < len) {
            S.stats.tx_bytes_to_clients += (uint32_t)w;
        } else {
            S.stats.tx_bytes_to_clients += len;
        }
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
    for (int i = 0; i < SINK_TCP_MAX_CLIENTS; i++) {
        tcp_client_t *c = &S.clients[i];
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
