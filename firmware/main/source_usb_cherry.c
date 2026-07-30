// SPDX-License-Identifier: GPL-2.0-or-later
//
// source_usb_cherry.c — USB-host source for CDC2NET on the CherryUSB stack.
//
// Same contract as source_usb.c (the esp-usb implementation): a transparent
// byte pipe from the attached stick to the bridge.  Only the underlying host
// stack differs, so both files implement the identical source_ops vtable and
// exactly one of them is built (Kconfig choice CDC2NET_SOURCE_USB*).
//
// Why a second implementation instead of porting in place:
//   - esp-usb (espressif/usb) has NO transaction-translator support, so a
//     full-speed stick behind a high-speed hub is rejected outright
//     ("transaction translator (TT) is not supported").  That blocks the
//     multi-stick case on any SoC whose root port runs at high speed
//     (ESP32-S31: UTMI-only PHY, no full-speed PHY at all).
//   - CherryUSB implements split transactions, so FS sticks work behind an
//     HS hub, and it ships CDC-ACM/FTDI/CH34x/CP210x class drivers in-tree —
//     replacing the three vendored *_vcp components on this path.
// Bench-proven on ESP32-S31: hub + CUL (cdc_acm) + C6 (cdc_acm) + CP2102N
// (cp210x) enumerated together, with bidirectional bulk traffic over splits.
//
// Single-stick semantics are kept identical to source_usb.c: the first serial
// device to come up owns the pipe, later enumerations are ignored until it
// disconnects.  (Multi-strand — one bridge per stick — is a separate step;
// the point of this file is the stack swap, not N:1.)

#include "source_usb.h"
#include "bridge.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "usbh_core.h"
#include "usbh_serial.h"
#include "serialcfg.h"        // per-device line-coding store (Layer A)

static const char *TAG = "src-usb-cherry";

// Root-port register base.  The ESP32-S31 exposes only the high-speed OTG
// controller (no FS PHY); S2/S3 have only the full-speed one.
#if defined(CONFIG_IDF_TARGET_ESP32S31) || defined(CONFIG_IDF_TARGET_ESP32P4)
#define CDC2NET_USB_BASE  ESP_USB_HS0_BASE
#else
#define CDC2NET_USB_BASE  ESP_USB_FS0_BASE
#endif

#define RX_CHUNK      512
#define RX_TIMEOUT_MS 100     // read() poll interval; also bounds disconnect latency

static struct {
    source_t             source;
    struct usbh_serial  *ser;              // current handle, NULL when closed
    SemaphoreHandle_t    connect_sem;      // given by the event handler
    SemaphoreHandle_t    tx_mtx;           // serialize op_tx() / line-coding callers
    volatile bool        ready;
    volatile bool        connected;
    volatile bool        want_close;       // set by the disconnect event
    volatile bool        claimed;          // a device is ours from bind to teardown
    volatile bool        rx_idle;          // true when stick_task touches no usbh_serial handle
    uint8_t              hub_index;        // where the claimed device sits, so a
    uint8_t              hub_port;         // sibling's disconnect isn't taken as ours
    char                 devname[CONFIG_USBHOST_DEV_NAMELEN];
    uint16_t             vid;
    uint16_t             pid;
    char                 manuf[48];
    char                 product[48];
    char                 serial[SERIALCFG_KEY_LEN];   // iSerialNumber ("" if none)
    char                 key[SERIALCFG_KEY_LEN];      // serial else "VVVV:PPPP"
    bool                 is_vcp;           // opened via a real-UART VCP driver
    const char          *kind;             // driver name (cdc_acm/cp210x/…), NULL when closed
    // Line-coding display shadow, CDC encoding (same as source_usb.c):
    // bits 5..8, parity 0N/1O/2E/3M/4S, stop 0=1/1=1.5/2=2.
    uint32_t             lc_baud;
    uint8_t              lc_bits, lc_parity, lc_stop;
    uint8_t              lc_source;        // 0 default / 1 nvs / 2 rfc2217
    uint32_t             rx_bytes;
    uint32_t             tx_bytes;
    char                 describe_buf[48];
    uint8_t              rx_buf[RX_CHUNK];
} S;

static void copy_str(char *out, size_t cap, const char *in)
{
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", in ? in : "");
}

// Derive the per-device key: iSerialNumber (sanitised) if present, else
// "VVVV:PPPP".  Identical rule to source_usb.c so NVS entries written on the
// esp-usb build keep matching after the stack swap.
static void derive_key(uint16_t vid, uint16_t pid, char *out, size_t cap)
{
    if (S.serial[0]) {
        size_t o = 0;
        for (const char *p = S.serial; *p && o + 1 < cap; p++) {
            char c = *p;
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            out[o++] = ok ? c : '_';
        }
        out[o] = '\0';
    } else {
        snprintf(out, cap, "%04X:%04X", vid, pid);
    }
}

// Fetch a USB string descriptor into `out` ("" if the device has none or the
// request fails).
//
// CherryUSB 1.6.1 declares hport->iManufacturer/iProduct/iSerialNumber but
// NEVER assigns them — the fields are dead, so reading them yields NULL.  Its
// CONFIG_USBHOST_GET_STRING_DESC is no help either: that only *logs* the
// strings into a local buffer, and it `goto errout`s the entire enumeration if
// a device stalls a string request — i.e. enabling it would let a quirky stick
// fail to enumerate at all.  So we ask for the strings ourselves and treat any
// failure as "no string".
//
// Must run on the hub thread: usbh_get_string_desc() writes through the
// bus-global ep0_request_buffer, and usbh_control_transfer() only locks the
// per-port mutex — so it is serialized against the core's own enumeration
// transfers by thread affinity, not by a lock.  The core fires events with that
// mutex released, so this neither deadlocks nor races.
static void fetch_str(struct usbh_hubport *hport, uint8_t index,
                      char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!index) return;                     // device declares no such string

    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));            // get_string_desc does not NUL-terminate
    if (usbh_get_string_desc(hport, index, buf, sizeof(buf) - 1) < 0) return;
    buf[sizeof(buf) - 1] = '\0';
    snprintf(out, cap, "%s", (char *)buf);
}

// ── Teardown handshake (hub thread context) ──────────────────────────────
// CherryUSB calls this __WEAK hook from usbh_serial_remove(), and it is the
// ONLY callback that runs BEFORE usbh_serial_free() destroys the instance's
// rx_complete_sem.  Both USB events fire too late to help: usbh_hubport_release()
// runs CLASS_DISCONNECT (-> usbh_serial_remove -> free) FIRST and only then
// emits INTERFACE_STOP / DEVICE_DISCONNECTED.  So a want_close driven purely by
// the disconnect event cannot save an RX task that is already parked inside
// usbh_serial_read() -> usb_osal_sem_take(rx_complete_sem): the semaphore gets
// deleted under it, and the wakeup lands in xQueueSemaphoreTake on freed queue
// memory -> "assert failed: spinlock_acquire ... (lock->count == 0)" -> panic.
//
// The scheduling makes this race easy to lose: CherryUSB's IDF osal inverts
// priorities (usb_osal_thread_create: configMAX_PRIORITIES - 1 - prio), so
// CONFIG_USBHOST_PSC_PRIO=0 makes the hub thread the highest-priority task in
// the system while stick_task runs at 4.  remove() does offer one scheduling
// window before the free (usb_osal_thread_schedule_other() drops to idle
// priority and yields once), but that single window is not a synchronization
// point — on this build every unplug panicked without the explicit handshake
// below.  So we don't rely on scheduling luck: block the hub thread here until
// the RX task confirms it has let go of the handle.
//
// (usbh_serial_free() does not release the struct itself — g_serial_class[] is
// static — so the semaphore is the only object with a lifetime problem.)
void usbh_serial_stop(struct usbh_serial *serial)
{
    if (!serial || !S.claimed) return;

    // Ours?  Match the handle once opened, else the devname we claimed
    // (stick_task may still be in its open-retry loop).
    bool mine = (serial == S.ser);
    if (!mine && serial->hport) {
        const char *dn = serial->hport->config.intf[serial->intf].devname;
        mine = (dn && S.devname[0] && strcmp(dn, S.devname) == 0);
    }
    if (!mine) return;

    S.want_close = true;

    // Keep op_tx()/apply_lc() off the handle from here on.  They only touch
    // S.ser under tx_mtx, so once this returns nobody can enter with it again.
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    S.ser = NULL;
    xSemaphoreGive(S.tx_mtx);

    // Now wait for the RX loop to actually let go of `serial`.  The delay MUST
    // be >= 1 tick: at CONFIG_FREERTOS_HZ=100 pdMS_TO_TICKS(5) is 0 ticks, and
    // vTaskDelay(0) is a bare yield — which, on the highest-priority task in
    // the system, never hands the CPU to stick_task (measured: the loop fell
    // straight through and the free still won).  pdMS_TO_TICKS(10) is 1 tick.
    //
    // Budget: the RX task is back at its want_close check within ~100 ms
    // (rx_timeout) plus whatever one rx_sink fanout takes — worst case ~100 ms
    // per stalled telnet client (tn_send gives up and closes after ~50 retries)
    // x 4 clients.  5 s covers that pathological stack several times over; the
    // normal case exits after one or two 10 ms polls.  If it ever expires, the
    // free proceeds and the known deleted-semaphore panic is back on the table
    // — hence the loud log.
    for (int i = 0; i < 500 && !S.rx_idle; i++)
        vTaskDelay(pdMS_TO_TICKS(10));           // <= ~5 s, really suspends
    if (!S.rx_idle)
        ESP_LOGE(TAG, "RX task still holds %s at free — deleted-semaphore panic likely",
                 S.devname);
}

// ── Host event handler (hub thread context) ──────────────────────────────
// Runs on CherryUSB's hub thread: record identity + hand the device name to
// stick_task.  No blocking work here.  All events arrive on that one thread,
// so the claim below needs no lock.
//
// Bind on INTERFACE_START, NOT on DEVICE_CONFIGURED: the core fires
// DEVICE_CONFIGURED with intf == USB_INTERFACE_ANY (0xff) *before* it loads any
// class driver, so there is no interface index to read and no devname yet — the
// serial class only registers /dev/ttyACMx from inside its CLASS_CONNECT.
// INTERFACE_START fires per interface right after that succeeded, with the real
// index (usbh_core.c: usbh_enumerate).
static void usbh_evt(uint8_t busid, uint8_t hub_index, uint8_t hub_port,
                     uint8_t intf, uint8_t event)
{
    switch (event) {
    case USBH_EVENT_INTERFACE_START: {
        if (S.claimed) return;        // single-stick: the first serial device wins
        struct usbh_hubport *hport = usbh_find_hubport(busid, hub_index, hub_port);
        if (!hport || !hport->connected) return;
        if (intf >= hport->config.config_desc.bNumInterfaces) return;

        const char *dn = hport->config.intf[intf].devname;
        // Only serial class instances carry a /dev/tty* name; skip hubs and
        // the non-serial interfaces of composite devices (e.g. the C6's
        // vendor-specific JTAG interface).
        if (!dn || strncmp(dn, "/dev/tty", 8) != 0) return;

        copy_str(S.devname, sizeof(S.devname), dn);
        S.vid = hport->device_desc.idVendor;
        S.pid = hport->device_desc.idProduct;
        // Identity strings drive the WebUI card AND derive_key(): without the
        // real iSerialNumber every device would silently key on "VVVV:PPPP",
        // so per-device serialcfg entries written by the esp-usb build would
        // stop matching after the stack swap.
        fetch_str(hport, hport->device_desc.iManufacturer, S.manuf,   sizeof(S.manuf));
        fetch_str(hport, hport->device_desc.iProduct,      S.product, sizeof(S.product));
        fetch_str(hport, hport->device_desc.iSerialNumber, S.serial,  sizeof(S.serial));
        S.hub_index = hub_index;
        S.hub_port  = hub_port;

        ESP_LOGW(TAG, "USB device %s VID=0x%04X PID=0x%04X manuf='%s' product='%s'",
                 S.devname, S.vid, S.pid, S.manuf, S.product);
        S.want_close = false;         // paired with the claim, never cleared by stick_task
        S.claimed    = true;
        if (S.connect_sem) xSemaphoreGive(S.connect_sem);
        break;
    }
    case USBH_EVENT_DEVICE_DISCONNECTED:
        // Fires with intf == USB_INTERFACE_ANY, so identity comes from the port:
        // only tear down when the device WE claimed is the one that went away —
        // a sibling stick behind a hub must not drop our pipe.
        if (S.claimed && hub_index == S.hub_index && hub_port == S.hub_port)
            S.want_close = true;
        break;
    default:
        break;
    }
}

// Apply line coding to the open device and update the display shadow.
// Serialized with op_tx() via tx_mtx (both touch S.ser).
//
// CherryUSB's SET_ATTR does three things at once: pushes SET_LINE_CODING via
// the class driver, asserts DTR+RTS, and re-arms the bulk-IN URB.  That is
// exactly the sequence source_usb.c does by hand (line coding + explicit
// set_control_line_state on the native path), so no separate DTR call here.
// src: 0 default / 1 nvs / 2 rfc2217.
static esp_err_t apply_lc(uint32_t baud, uint8_t bits,
                          uint8_t parity, uint8_t stop, uint8_t src)
{
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (S.ser) {
        struct usbh_serial_termios tio = {
            .baudrate   = baud,
            .databits   = bits,
            .parity     = parity,
            .stopbits   = stop,
            .rtscts     = false,
            .rx_timeout = RX_TIMEOUT_MS,
        };
        int ret = usbh_serial_control(S.ser, USBH_SERIAL_CMD_SET_ATTR, &tio);
        if (ret < 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        S.lc_baud   = baud;
        S.lc_bits   = bits;
        S.lc_parity = parity;
        S.lc_stop   = stop;
        S.lc_source = src;
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

static void stick_task(void *arg)
{
    (void)arg;

    while (1) {
        xSemaphoreTake(S.connect_sem, portMAX_DELAY);

        // Claim BEFORE the first handle access, not after a successful open:
        // a yank landing between usbh_serial_open() returning and a later
        // rx_idle=false would let usbh_serial_stop() sail through and free the
        // instance under us.
        S.rx_idle = false;

        struct usbh_serial *ser = usbh_serial_open(S.devname, USBH_SERIAL_O_RDWR);
        if (!ser) {
            // Retry for as long as the claimed device stays plugged in: no
            // further event is coming for a device that is already enumerated,
            // so giving up here would park this task until the next hot-plug.
            // The disconnect event sets want_close and releases us.
            ESP_LOGW(TAG, "open %s failed — retrying while device is present", S.devname);
            while (!ser && !S.want_close) {
                vTaskDelay(pdMS_TO_TICKS(500));
                ser = usbh_serial_open(S.devname, USBH_SERIAL_O_RDWR);
            }
        }
        if (!ser) {              // vanished while we were retrying
            S.rx_idle = true;
            S.claimed = false;   // re-arm: the next enumeration may claim
            continue;
        }

        xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
        S.ser = ser;
        xSemaphoreGive(S.tx_mtx);

        S.connected  = true;
        S.kind   = (ser->driver && ser->driver->driver_name) ? ser->driver->driver_name : "serial";
        S.is_vcp = (strcmp(S.kind, "cdc_acm") != 0);   // anything else drives a real UART
        derive_key(S.vid, S.pid, S.key, sizeof(S.key));

        // Resolve this device's line coding: per-device NVS entry, else the
        // global default.  Unlike the esp-usb path this is pushed for native
        // CDC too — CherryUSB needs a non-zero dwDTERate before read() will
        // run, and it arms the RX URB as a side effect.
        serialcfg_lc_t scl;
        uint8_t lc_src;
        if (serialcfg_lookup(S.key, &scl)) {
            lc_src = 1;
        } else {
            scl = serialcfg_default();
            lc_src = 0;
        }
        if (apply_lc(scl.baud, scl.bits, scl.parity, scl.stop, lc_src) != ESP_OK) {
            ESP_LOGW(TAG, "line coding set failed — closing");
            xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
            S.ser = NULL;
            xSemaphoreGive(S.tx_mtx);
            usbh_serial_close(ser);
            S.rx_idle   = true;
            S.connected = false;
            S.kind = NULL;
            S.claimed = false;
            continue;
        }

        S.ready = true;
        ESP_LOGW(TAG, "%s open %s (VID=0x%04X PID=0x%04X) key='%s' %u%c%u@%u — source ready",
                 S.kind, S.devname, S.vid, S.pid, S.key, S.lc_bits,
                 "NOEMS"[S.lc_parity <= 4 ? S.lc_parity : 0],
                 S.lc_stop == 0 ? 1 : 2, (unsigned)S.lc_baud);

        // RX loop: read() blocks up to rx_timeout, so a disconnect is noticed
        // within RX_TIMEOUT_MS even on a silent link.
        while (!S.want_close) {
            int n = usbh_serial_read(ser, S.rx_buf, sizeof(S.rx_buf));
            if (n > 0) {
                S.rx_bytes += (uint32_t)n;
                if (S.source.rx_sink)
                    S.source.rx_sink(S.source.rx_sink_ctx, S.rx_buf, (size_t)n);
            } else if (n < 0 && n != -USB_ERR_TIMEOUT) {
                // A read error does NOT imply the stick is gone.  Re-configuring
                // the line from another task (apply_lc -> SET_ATTR, i.e. every
                // RFC2217 SET-* and the initial open) calls usbh_kill_urb() on
                // the in-flight bulk-IN, which completes this read with
                // -USB_ERR_SHUTDOWN before SET_ATTR re-arms it.  Tearing the
                // source down on that would drop every TCP client on the first
                // baud change and — since a still-plugged device raises no new
                // enumeration event — never reopen.  Only trust the host: the
                // disconnect event drives want_close, and this mirrors the
                // library's own liveness test as a backstop.
                if (!ser->hport || !ser->hport->connected || ser->ref_count == 0)
                    break;
                vTaskDelay(pdMS_TO_TICKS(10));   // transient — yield, don't spin
            }
            // -USB_ERR_TIMEOUT (idle link) and empty reads just loop.
        }

        S.ready     = false;
        S.connected = false;
        xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
        S.ser = NULL;
        xSemaphoreGive(S.tx_mtx);
        S.is_vcp    = false;
        S.kind      = NULL;
        S.key[0]    = '\0';
        S.serial[0] = '\0';
        usbh_serial_close(ser);
        // Last use of `ser` — release usbh_serial_stop() so CherryUSB may free
        // the instance.  (A close() on an already-closed handle is a no-op:
        // remove() closed it before calling us, ref_count is 0 by now.)
        S.rx_idle = true;
        ESP_LOGI(TAG, "source closed; waiting for reconnect");
        // Device is gone — sinks must drop per-device state so downstream
        // (FHEM) reconnects and re-initialises against the next stick.  The
        // bridge cannot tell a swap from a reconnect (C3/C6 share 303A:1001).
        bridge_notify_source_down();
        // Release last, once our state has settled, so an incoming enumeration
        // cannot have its identity clobbered by the resets above.  Note this
        // re-arms on the next INTERFACE_START only: a second stick that was
        // already enumerated behind a hub when ours was pulled raises no fresh
        // event and stays unbound until it is replugged.  Acceptable while this
        // source is single-stick by contract (see the file header).
        S.claimed = false;
    }
}

// ── source_t hooks ───────────────────────────────────────────────────────

static esp_err_t op_tx(source_t *src, const uint8_t *data, size_t len)
{
    (void)src;
    if (!data || !len) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    struct usbh_serial *ser = S.ser;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (ser) {
        int n = usbh_serial_write(ser, data, len);
        if (n == (int)len) {
            S.tx_bytes += (uint32_t)len;
            err = ESP_OK;
        } else {
            err = (n < 0) ? ESP_FAIL : ESP_ERR_TIMEOUT;   // short write
        }
    }
    xSemaphoreGive(S.tx_mtx);
    return err;
}

static bool op_ready(source_t *src) { (void)src; return S.ready; }

// set: an RFC2217 controller issued SET-*.  Stamp src=2 so the WebUI/NVS path
// knows a live session owns the wire.
static esp_err_t op_set_line_coding(source_t *src, uint32_t baud, uint8_t bits,
                                    uint8_t parity, uint8_t stop)
{
    (void)src;
    return apply_lc(baud, bits, parity, stop, 2 /* rfc2217 */);
}

static esp_err_t op_apply_line_coding(source_t *src, uint32_t baud, uint8_t bits,
                                      uint8_t parity, uint8_t stop, uint8_t lc_src)
{
    (void)src;
    return apply_lc(baud, bits, parity, stop, lc_src);
}

// revert: the RFC2217 controller released — re-apply this device's NVS entry
// (else the global default).  Snapshot the key under tx_mtx and release it
// before apply_lc() (non-recursive mutex), same rule as source_usb.c.
static void op_revert_line_coding(source_t *src)
{
    (void)src;
    char key[SERIALCFG_KEY_LEN];
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    snprintf(key, sizeof(key), "%s", S.key);
    xSemaphoreGive(S.tx_mtx);

    serialcfg_lc_t scl;
    uint8_t lc_src;
    if (key[0] && serialcfg_lookup(key, &scl)) {
        lc_src = 1;
    } else {
        scl = serialcfg_default();
        lc_src = 0;
    }
    apply_lc(scl.baud, scl.bits, scl.parity, scl.stop, lc_src);
}

static void op_get_line_coding(source_t *src, uint32_t *baud, uint8_t *bits,
                               uint8_t *parity, uint8_t *stop)
{
    (void)src;
    xSemaphoreTake(S.tx_mtx, portMAX_DELAY);
    if (baud)   *baud   = S.lc_baud;
    if (bits)   *bits   = S.lc_bits;
    if (parity) *parity = S.lc_parity;
    if (stop)   *stop   = S.lc_stop;
    xSemaphoreGive(S.tx_mtx);
}

static esp_err_t op_reset(source_t *src)
{
    (void)src;
    // No VBUS FET on the bringup board (passive 5V tie) -> no power-cycle.
    return ESP_ERR_NOT_SUPPORTED;
}

static const char *op_describe(source_t *src)
{
    (void)src;
    snprintf(S.describe_buf, sizeof(S.describe_buf), "%s %04X:%04X %s",
             S.kind ? S.kind : "USB", S.vid, S.pid, S.connected ? "open" : "closed");
    return S.describe_buf;
}

static void op_get_stats(source_t *src, source_stats_t *out)
{
    (void)src;
    if (!out) return;
    out->connected = S.connected;
    out->vid       = S.vid;
    out->pid       = S.pid;
    out->rx_bytes  = S.rx_bytes;
    out->tx_bytes  = S.tx_bytes;
    snprintf(out->manuf,   sizeof(out->manuf),   "%s", S.manuf);
    snprintf(out->product, sizeof(out->product), "%s", S.product);
}

static void op_get_serial_info(source_t *src, source_serial_info_t *out)
{
    (void)src;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->connected = S.connected;
    out->is_vcp    = S.is_vcp;
    out->vid       = S.vid;
    out->pid       = S.pid;
    out->baud      = S.lc_baud;
    out->bits      = S.lc_bits;
    out->parity    = S.lc_parity;
    out->stop      = S.lc_stop;
    out->lc_source = S.lc_source;
    snprintf(out->serial, sizeof(out->serial), "%s", S.serial);
    snprintf(out->key,    sizeof(out->key),    "%s", S.key);
}

static const struct source_ops s_ops = {
    .tx                 = op_tx,
    .ready              = op_ready,
    .reset              = op_reset,
    .describe           = op_describe,
    .set_line_coding    = op_set_line_coding,
    .revert_line_coding = op_revert_line_coding,
    .get_line_coding    = op_get_line_coding,
    .apply_line_coding  = op_apply_line_coding,
    .get_stats          = op_get_stats,
    .get_serial_info    = op_get_serial_info,
};

source_t *source_usb_init(void)
{
    memset(&S, 0, sizeof(S));
    S.rx_idle         = true;    // nothing held yet (memset cleared it)
    S.connect_sem     = xSemaphoreCreateBinary();
    S.tx_mtx          = xSemaphoreCreateMutex();
    S.source.ops      = &s_ops;
    S.source.short_id = "usb";

    // Brings up the PHY, the host controller and CherryUSB's hub thread; the
    // registered class drivers (CDC-ACM/FTDI/CH34x/CP210x) bind automatically.
    usbh_initialize(0, CDC2NET_USB_BASE, usbh_evt);
    xTaskCreate(stick_task, "stick", 6144, NULL, 4, NULL);

    ESP_LOGI(TAG, "CherryUSB host + serial source installed");
    return &S.source;
}
