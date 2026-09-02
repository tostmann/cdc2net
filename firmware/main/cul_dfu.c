// SPDX-License-Identifier: GPL-2.0-or-later
//
// cul_dfu.c — Atmel-DFU-Klient auf dem USB-Wirt.
//
// PROTOKOLLQUELLE
// ---------------
// Die Kommandobytes und der Aufbau eines Schreibblocks stammen NICHT aus dem
// Gedaechtnis, sondern aus dfu-programmer (GPL-2.0-or-later, also mit diesem
// Baum vertraeglich), Dateien src/atmel.c und src/dfu.c, die sich ihrerseits
// auf Atmels Anwendungsnotiz doc7618 berufen. Uebernommen sind die Tatsachen
// des Protokolls, nicht der Programmtext. Die Fundstellen stehen jeweils an
// der Funktion.
//
// Wesentliche Punkte, an denen ein Nachbau aus dem Bauch schiefgeht:
//   * Der Schreibblock ist NICHT einfach die Nutzdaten. Er besteht aus einem
//     32 Byte langen Steuerblock (davon nur 6 belegt, der Rest Null), den
//     Daten, und einem 16 Byte langen Anhang in DFU-Form.
//   * Die Adressen im Kopf sind MODULO 64 KB angegeben.
//   * Jeder Steuertransfer traegt eine hochzaehlende Vorgangsnummer in wValue.
//   * Nach jedem Schreiben muss der Zustand abgefragt werden; das Loeschen
//     dauert deutlich laenger als ein normaler Transfer.
//
// WARUM NUR ENDPUNKT 0
// --------------------
// Das gesamte Protokoll laeuft ueber Steuertransfers. Deshalb braucht dieses
// Modul keinen der esp-usb-Klassentreiber, sondern nur einen eigenen
// Wirt-Klienten, der das Geraet oeffnet und Schnittstelle 0 belegt.

#include "cul_dfu.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "cul-dfu";

// ---- DFU-Klasse (USB DFU 1.1) --------------------------------------------
#define DFU_DNLOAD     1
#define DFU_UPLOAD     2
#define DFU_GETSTATUS  3
#define DFU_CLRSTATUS  4

// dfu.c: dfu_transfer_out/in — Klasse, Empfaenger Schnittstelle.
#define DFU_REQ_OUT   (USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS \
                       | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)
#define DFU_REQ_IN    (USB_BM_REQUEST_TYPE_DIR_IN  | USB_BM_REQUEST_TYPE_TYPE_CLASS \
                       | USB_BM_REQUEST_TYPE_RECIP_INTERFACE)

#define DFU_STATUS_OK        0x00
#define DFU_STATE_DFU_ERROR  0x0A

// ---- Atmel-Rahmen (atmel.c) ----------------------------------------------
#define ATMEL_CONTROL_BLOCK_SIZE  32      // atmel.c: ATMEL_CONTROL_BLOCK_SIZE
#define ATMEL_FOOTER_SIZE         16      // atmel.c: ATMEL_FOOTER_SIZE
#define ATMEL_MAX_TRANSFER        0x400   // atmel.c: ATMEL_MAX_TRANSFER_SIZE
#define ATMEL_64KB_PAGE           0x10000 // atmel.c: ATMEL_64KB_PAGE

#define ATMEL_UNIT_EEPROM  0x02   // atmel.c: command[1] beim Lesen
#define EEPROM_GUARD_BYTES 256    // so viel wird verglichen

#define DFU_IFACE       0
#define CTRL_BUF_BYTES  (sizeof(usb_setup_packet_t) + ATMEL_CONTROL_BLOCK_SIZE \
                         + ATMEL_MAX_TRANSFER + ATMEL_FOOTER_SIZE)

static struct {
    usb_host_client_handle_t client;
    usb_device_handle_t      dev;
    SemaphoreHandle_t        done;      // Abschluss eines Steuertransfers
    SemaphoreHandle_t        lock;      // serialisiert cul_dfu_flash()
    usb_transfer_t          *xfer;
    uint16_t                 transaction;
    cul_dfu_status_t         st;
} S;

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(S.st.last, sizeof(S.st.last), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", S.st.last);
}

// ---- Steuertransfer -------------------------------------------------------

static void xfer_done_cb(usb_transfer_t *t)
{
    (void)t;
    xSemaphoreGive(S.done);
}

// Ein Steuertransfer, blockierend. `data` darf NULL sein (Laenge 0).
static esp_err_t ctrl(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                      const uint8_t *out, uint8_t *in, uint16_t len,
                      uint32_t timeout_ms)
{
    if (!S.dev || !S.xfer) return ESP_ERR_INVALID_STATE;
    if (len > CTRL_BUF_BYTES - sizeof(usb_setup_packet_t)) return ESP_ERR_INVALID_SIZE;

    usb_setup_packet_t *sp = (usb_setup_packet_t *)S.xfer->data_buffer;
    sp->bmRequestType = bmRequestType;
    sp->bRequest      = bRequest;
    sp->wValue        = wValue;
    sp->wIndex        = DFU_IFACE;
    sp->wLength       = len;

    if (out && len) memcpy(S.xfer->data_buffer + sizeof(*sp), out, len);

    S.xfer->device_handle    = S.dev;
    S.xfer->bEndpointAddress = 0;
    S.xfer->num_bytes        = sizeof(*sp) + len;
    S.xfer->callback         = xfer_done_cb;
    S.xfer->context          = NULL;

    xSemaphoreTake(S.done, 0);          // altes Signal abraeumen
    esp_err_t e = usb_host_transfer_submit_control(S.client, S.xfer);
    if (e != ESP_OK) return e;

    if (xSemaphoreTake(S.done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (S.xfer->status != USB_TRANSFER_STATUS_COMPLETED)
        return ESP_FAIL;

    if (in && len) memcpy(in, S.xfer->data_buffer + sizeof(*sp), len);
    return ESP_OK;
}

// dfu.c: dfu_download() — wValue traegt die hochzaehlende Vorgangsnummer.
static esp_err_t dfu_dnload(const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    return ctrl(DFU_REQ_OUT, DFU_DNLOAD, S.transaction++, data, NULL, len, timeout_ms);
}

// dfu.c: dfu_get_status() — sechs Byte: Status, Wartezeit(3), Zustand, Text.
static esp_err_t dfu_getstatus(uint8_t *bStatus, uint8_t *bState, uint32_t *poll_ms)
{
    uint8_t b[6] = { 0 };
    esp_err_t e = ctrl(DFU_REQ_IN, DFU_GETSTATUS, 0, NULL, b, sizeof(b), 2000);
    if (e != ESP_OK) return e;
    if (bStatus) *bStatus = b[0];
    if (bState)  *bState  = b[4];
    if (poll_ms) *poll_ms = (uint32_t)b[1] | ((uint32_t)b[2] << 8) | ((uint32_t)b[3] << 16);
    return ESP_OK;
}

// dfu.c: dfu_upload() — traegt ebenfalls eine Vorgangsnummer.
static esp_err_t dfu_upload(uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    return ctrl(DFU_REQ_IN, DFU_UPLOAD, S.transaction++, NULL, data, len, timeout_ms);
}

static esp_err_t dfu_clrstatus(void)
{
    return ctrl(DFU_REQ_OUT, DFU_CLRSTATUS, 0, NULL, NULL, 0, 2000);
}

// Zustand abfragen und auf ein Ergebnis warten. Das Loeschen des Bausteins
// braucht mehr als eine Sekunde, deshalb die grosszuegige Schranke.
static esp_err_t wait_ok(uint32_t total_ms)
{
    const uint32_t step = 100;
    for (uint32_t waited = 0; waited <= total_ms; waited += step) {
        uint8_t status = 0xFF, state = 0xFF;
        uint32_t poll = 0;
        esp_err_t e = dfu_getstatus(&status, &state, &poll);
        if (e != ESP_OK) return e;
        if (status == DFU_STATUS_OK) return ESP_OK;
        if (state == DFU_STATE_DFU_ERROR) {
            dfu_clrstatus();
            note("bootloader reports error 0x%02X", status);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(poll > step ? poll : step));
    }
    note("timed out waiting for the bootloader");
    return ESP_ERR_TIMEOUT;
}

// ---- Atmel-Befehle --------------------------------------------------------

// atmel.c: atmel_erase_flash(), command[3] = { 0x04, 0x00, <modus> }.
// Der Modus waehlt, WAS geloescht wird:
//   0x00  Programmspeicher  0..16K
//   0x20  Programmspeicher 16..32K
//   0x40 / 0x80             32..48K / 48..64K
//   0xFF  der GANZE Baustein
//
// Hier wird bewusst BLOCKWEISE geloescht und nur so weit, wie das Abbild
// reicht — nicht „ganzer Baustein".
//
// Grund: das EEPROM des Sticks traegt seine Konfiguration und darf dabei nicht
// verlorengehen. Gemessen an einem busware CUL868 ueberstehen drei Marken auch
// ein 0xFF-Loeschen unveraendert; das haengt aber an einer Sicherung im
// Baustein, die sich hier nicht auslesen laesst (der Bootlader dieses Sticks
// verweigert jedes Lesen — dieselbe Sperre, an der auch `dfu-programmer dump`
// scheitert). Auf ein Verhalten zu bauen, das man nicht nachpruefen kann, ist
// genau das, was man nicht tun sollte: also den Auftrag so eng fassen, dass die
// Frage gar nicht erst entsteht.
//
// Wird der Blockmodus abgelehnt, faellt die Funktion auf 0xFF zurueck — aber
// SICHTBAR, in der Statusmeldung, nicht stillschweigend.
static esp_err_t atmel_erase_blocks(size_t img_len, bool *fell_back)
{
    static const uint8_t modes[4] = { 0x00, 0x20, 0x40, 0x80 };
    const int need = (int)((img_len + 0x3FFF) / 0x4000);   // 16-KB-Bloecke
    *fell_back = false;

    for (int i = 0; i < need && i < 4; i++) {
        uint8_t cmd[3] = { 0x04, 0x00, modes[i] };
        if (dfu_dnload(cmd, sizeof(cmd), 5000) != ESP_OK || wait_ok(20000) != ESP_OK) {
            uint8_t full[3] = { 0x04, 0x00, 0xFF };
            ESP_LOGW(TAG, "Blockloeschen abgelehnt — weiche auf den ganzen Baustein aus");
            *fell_back = true;
            esp_err_t e = dfu_dnload(full, sizeof(full), 5000);
            if (e != ESP_OK) return e;
            return wait_ok(20000);
        }
    }
    return ESP_OK;
}

// atmel.c: atmel_start_app_reset(), command[3] = { 0x04, 0x03, 0x00 },
// danach ein Download der Laenge NULL. Der Baustein startet daraufhin neu und
// meldet sich vom Bus ab — eine Antwort darauf gibt es also nicht mehr.
static esp_err_t atmel_start_app(void)
{
    static const uint8_t cmd[3] = { 0x04, 0x03, 0x00 };
    esp_err_t e = dfu_dnload(cmd, sizeof(cmd), 2000);
    if (e != ESP_OK) return e;
    dfu_dnload(NULL, 0, 2000);       // Rueckmeldung ist hier nicht zu erwarten
    return ESP_OK;
}

// atmel.c: __atmel_flash_block() + atmel_flash_populate_header/footer().
// Aufbau: 32 Byte Steuerblock | Daten | 16 Byte Anhang.
static esp_err_t atmel_flash_block(uint32_t start, const uint8_t *data, uint16_t len)
{
    static uint8_t msg[ATMEL_CONTROL_BLOCK_SIZE + ATMEL_MAX_TRANSFER + ATMEL_FOOTER_SIZE];
    const uint32_t end = start + len - 1;

    memset(msg, 0, ATMEL_CONTROL_BLOCK_SIZE);

    // Kopf. Die Adressen stehen modulo 64 KB.
    msg[0] = 0x01;                                   // ld_prog_start
    msg[1] = 0x00;                                   // 0 = Programmspeicher
    msg[2] = (uint8_t)((start % ATMEL_64KB_PAGE) >> 8);
    msg[3] = (uint8_t)(start % ATMEL_64KB_PAGE);
    msg[4] = (uint8_t)((end % ATMEL_64KB_PAGE) >> 8);
    msg[5] = (uint8_t)(end % ATMEL_64KB_PAGE);

    memcpy(msg + ATMEL_CONTROL_BLOCK_SIZE, data, len);

    // Anhang in DFU-Form. dfu-programmer setzt die Pruefsumme auf Null und die
    // Kennungen auf 0xFFFF; der Bootlader wertet beides nicht aus.
    uint8_t *f = msg + ATMEL_CONTROL_BLOCK_SIZE + len;
    memset(f, 0, ATMEL_FOOTER_SIZE);
    f[4] = 16; f[5] = 'D'; f[6] = 'F'; f[7] = 'U';
    f[8] = 0x01; f[9] = 0x10;
    f[10] = f[11] = f[12] = f[13] = f[14] = f[15] = 0xFF;

    const uint16_t total = ATMEL_CONTROL_BLOCK_SIZE + len + ATMEL_FOOTER_SIZE;
    esp_err_t e = dfu_dnload(msg, total, 5000);
    if (e != ESP_OK) return e;
    return wait_ok(5000);
}

// atmel.c: __atmel_read_page(), command[6] = { 0x03, 0x00, start16, end16 };
// fuer das EEPROM eines AVR steht an command[1] eine 0x02 statt der 0x00.
// Danach die Bytes per Upload abholen.
static esp_err_t atmel_read(uint8_t unit, uint16_t start, uint8_t *out, uint16_t len)
{
    uint8_t cmd[6] = { 0x03, unit, 0, 0, 0, 0 };
    const uint16_t end = start + len - 1;
    cmd[2] = (uint8_t)(start >> 8); cmd[3] = (uint8_t)start;
    cmd[4] = (uint8_t)(end   >> 8); cmd[5] = (uint8_t)end;

    esp_err_t e = dfu_dnload(cmd, sizeof(cmd), 2000);
    if (e != ESP_OK) return e;
    return dfu_upload(out, len, 5000);
}

// ---- USB-Wirt-Klient ------------------------------------------------------

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        if (S.dev) break;                       // wir bedienen genau eines
        usb_device_handle_t dev = NULL;
        if (usb_host_device_open(S.client, msg->new_dev.address, &dev) != ESP_OK) break;

        const usb_device_desc_t *d = NULL;
        if (usb_host_get_device_descriptor(dev, &d) != ESP_OK || !d
            || d->idVendor != CUL_DFU_VID || d->idProduct != CUL_DFU_PID) {
            // Nicht unser Bootlader — wieder loslassen, damit der
            // CDC-Klassentreiber das Geraet bekommt.
            usb_host_device_close(S.client, dev);
            break;
        }
        if (usb_host_interface_claim(S.client, dev, DFU_IFACE, 0) != ESP_OK) {
            usb_host_device_close(S.client, dev);
            note("cannot claim interface 0");
            break;
        }
        S.dev = dev;
        S.st.present = true;
        S.st.vid = d->idVendor;
        S.st.pid = d->idProduct;
        S.transaction = 0;
        note("bootloader attached (%04X:%04X)", S.st.vid, S.st.pid);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (S.dev) {
            usb_host_interface_release(S.client, S.dev, DFU_IFACE);
            usb_host_device_close(S.client, S.dev);
            S.dev = NULL;
        }
        S.st.present = false;
        break;
    default:
        break;
    }
}

static void dfu_task(void *arg)
{
    (void)arg;
    while (1) {
        usb_host_client_handle_events(S.client, portMAX_DELAY);
    }
}

esp_err_t cul_dfu_start(void)
{
    if (S.client) return ESP_OK;

    S.done = xSemaphoreCreateBinary();
    S.lock = xSemaphoreCreateMutex();
    if (!S.done || !S.lock) return ESP_ERR_NO_MEM;

    const usb_host_client_config_t cfg = {
        .is_synchronous    = false,
        .max_num_event_msg = 5,
        .async = { .client_event_callback = client_event_cb, .callback_arg = NULL },
    };
    esp_err_t e = usb_host_client_register(&cfg, &S.client);
    if (e != ESP_OK) return e;

    e = usb_host_transfer_alloc(CTRL_BUF_BYTES, 0, &S.xfer);
    if (e != ESP_OK) return e;

    if (xTaskCreate(dfu_task, "cul-dfu", 4096, NULL, 5, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "DFU-Klient bereit (wartet auf %04X:%04X)", CUL_DFU_VID, CUL_DFU_PID);
    return ESP_OK;
}

const cul_dfu_status_t *cul_dfu_status(void) { return &S.st; }

esp_err_t cul_dfu_flash(const uint8_t *img, size_t len)
{
    if (!img || len == 0) return ESP_ERR_INVALID_ARG;

    // Der Bootlader liegt oben im Speicher und ist die einzige Rueckfahrkarte.
    // Ein Abbild, das dorthin reicht, wird abgelehnt — nicht abgeschnitten:
    // ein stillschweigend gekuerztes Abbild waere eine halb geschriebene
    // Firmware, und das faellt erst beim naechsten Start auf.
    if (len > CUL_DFU_APP_LIMIT) {
        note("image too large (%u > %u bytes) — would hit the bootloader",
             (unsigned)len, (unsigned)CUL_DFU_APP_LIMIT);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!S.dev) {
        note("no bootloader on the bus — send `B01` to the stick first");
        return ESP_ERR_NOT_FOUND;
    }
    if (xSemaphoreTake(S.lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    S.st.busy = true;
    S.st.written = 0;
    S.st.eeprom_checked = false;
    S.st.eeprom_intact  = false;
    esp_err_t e;

    // EEPROM-Wache.
    //
    // Gemessen an einem busware CUL868 (12.08.2026): drei Marken geschrieben,
    // voller Chip-Erase plus 15736-Byte-Neuflash, alle drei unveraendert — das
    // Loeschen laesst das EEPROM hier also in Ruhe. Das ist aber ein BEFUND AN
    // EINEM STICK und keine Zusage: ob das EEPROM ein Chip-Erase uebersteht,
    // haengt an einer Sicherung im Baustein, und die ist von aussen nicht
    // abzulesen, ohne den Bootlader danach zu fragen.
    //
    // Deshalb wird hier gemessen statt vertraut: vorher lesen, hinterher
    // vergleichen. Faellt es doch weg, steht das hinterher in der Meldung,
    // statt dass jemand seine Konfiguration still verliert.
    static uint8_t ee_before[EEPROM_GUARD_BYTES];
    static uint8_t ee_after[EEPROM_GUARD_BYTES];
    bool ee_read_ok = (atmel_read(ATMEL_UNIT_EEPROM, 0, ee_before,
                                  EEPROM_GUARD_BYTES) == ESP_OK);
    if (!ee_read_ok) ESP_LOGW(TAG, "EEPROM vorab nicht lesbar — Wache entfaellt");

    bool erase_fell_back = false;
    note("erasing %u KB of program memory", (unsigned)((len + 1023) / 1024));
    e = atmel_erase_blocks(len, &erase_fell_back);
    if (e != ESP_OK) goto out;

    for (size_t off = 0; off < len; off += ATMEL_MAX_TRANSFER) {
        uint16_t n = (uint16_t)((len - off > ATMEL_MAX_TRANSFER)
                                ? ATMEL_MAX_TRANSFER : (len - off));
        e = atmel_flash_block((uint32_t)off, img + off, n);
        if (e != ESP_OK) {
            note("write error at 0x%04X", (unsigned)off);
            goto out;
        }
        S.st.written = off + n;
        if (!S.dev) { e = ESP_ERR_NOT_FOUND; note("bootloader disappeared"); goto out; }
    }

    if (ee_read_ok
        && atmel_read(ATMEL_UNIT_EEPROM, 0, ee_after, EEPROM_GUARD_BYTES) == ESP_OK) {
        S.st.eeprom_checked = true;
        S.st.eeprom_intact  = (memcmp(ee_before, ee_after, EEPROM_GUARD_BYTES) == 0);
        if (!S.st.eeprom_intact)
            ESP_LOGE(TAG, "EEPROM hat sich beim Flashen VERAENDERT");
    }

    note("%u bytes written, starting the application", (unsigned)S.st.written);
    e = atmel_start_app();

out:
    S.st.busy = false;
    if (e == ESP_OK) {
        if (S.st.eeprom_checked && !S.st.eeprom_intact)
            note("done: %u bytes written — WARNING: EEPROM changed",
                 (unsigned)S.st.written);
        else if (S.st.eeprom_checked)
            note("done: %u bytes written, EEPROM unchanged",
                 (unsigned)S.st.written);
        else if (erase_fell_back)
            note("done: %u bytes — whole chip erased, EEPROM not verifiable",
                 (unsigned)S.st.written);
        else
            note("done: %u bytes written, only program memory erased",
                 (unsigned)S.st.written);
    }
    xSemaphoreGive(S.lock);
    return e;
}
