// SPDX-License-Identifier: GPL-2.0-or-later
//
// eeprom_m24c32.c — M24C32 32-Kbit I2C serial EEPROM boot self-test.
//
// IDF (esp_driver_i2c) port of EULFW32's Arduino-Wire implementation
// (EULFW32/src/main.cpp::eeprom_init_and_test).  Same wire behaviour:
//   - 7-bit slave 0x50, 400 kHz, 2-byte big-endian internal address
//   - dedicated WE pin: HIGH = write-protected (locked), LOW = unlocked
//   - boot probe = NON-DESTRUCTIVE RMW on the last byte (read orig, write
//     orig^0xA5, verify, restore orig, verify) → state 0/1/2
//   - write completion via a fixed tW delay (vTaskDelay; M24C32 tW <= 5 ms)
//     plus the caller's readback as the real completion check.  NB: an
//     i2c_master_probe-based ACK-poll was tried first and failed on the write
//     side (false state=2) → replaced by the fixed delay (docs §3.5).
//
// Pins default to the busware EUL/TUL C6 map (SDA=22, SCL=23, WE=15 — the
// values EULFW32 uses unchanged on that board); override via -D if a carrier
// differs.  Built only when CONFIG_CDC2NET_EEPROM_M24C32 (see Kconfig.projbuild
// + src/CMakeLists.txt; enabled in sdkconfig.defaults.esp32c6).

#include "eeprom_m24c32.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // vTaskDelay — wait out the EEPROM write cycle
#include <string.h>

#ifndef EEPROM_I2C_SDA
#  define EEPROM_I2C_SDA   22
#endif
#ifndef EEPROM_I2C_SCL
#  define EEPROM_I2C_SCL   23
#endif
#ifndef EEPROM_I2C_WE
#  define EEPROM_I2C_WE    15
#endif
#ifndef EEPROM_I2C_PORT
#  define EEPROM_I2C_PORT  -1     // -1 = let the driver auto-pick a free port
#endif

#define EEPROM_I2C_ADDR         0x50
#define EEPROM_SIZE_BYTES       4096
#define EEPROM_PAGE_BYTES       32
#define EEPROM_TEST_ADDR        (EEPROM_SIZE_BYTES - 1)
#define EEPROM_CLOCK_HZ         400000
#define EEPROM_WRITE_TIMEOUT_MS 10
#define I2C_OP_TIMEOUT_MS       100

static const char *TAG = "eeprom";

static uint8_t                 s_state = 0;   // 0 absent / 1 RMW OK / 2 RMW fail
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static inline void we_lock(void)   { gpio_set_level(EEPROM_I2C_WE, 1); }
static inline void we_unlock(void) { gpio_set_level(EEPROM_I2C_WE, 0); }

// Random read: 2-byte big-endian internal address, repeated start, n data
// bytes.  i2c_master_transmit_receive issues the repeated start for us.
static bool ee_read(uint16_t addr, uint8_t *buf, size_t n)
{
    uint8_t a[2] = { (uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF) };
    return i2c_master_transmit_receive(s_dev, a, sizeof(a), buf, n,
                                       I2C_OP_TIMEOUT_MS) == ESP_OK;
}

// Single-page write (the self-test writes exactly 1 byte).  Caller guarantees
// WE is LOW.  The device ACKs the command bytes, then runs an internal write
// cycle (tW <= 5 ms) during which it NACKs everything; we simply wait it out
// with a fixed delay — the caller's readback is the real completion check, so
// no ACK-polling is needed (and it avoids a probe code-path on the write side).
static bool ee_write(uint16_t addr, const uint8_t *buf, size_t n)
{
    uint8_t frame[2 + 8];                       // addr + up to 8 data bytes
    if (n > sizeof(frame) - 2) return false;
    frame[0] = (uint8_t)(addr >> 8);
    frame[1] = (uint8_t)(addr & 0xFF);
    memcpy(&frame[2], buf, n);
    esp_err_t e = i2c_master_transmit(s_dev, frame, 2 + n, I2C_OP_TIMEOUT_MS);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "  write transmit failed @0x%04X: %s", addr, esp_err_to_name(e));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(EEPROM_WRITE_TIMEOUT_MS));   // wait out tW
    return true;
}

void eeprom_init_and_test(void)
{
    // WE → output, locked (write-protected) before the bus comes up.
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << EEPROM_I2C_WE,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    we_lock();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = EEPROM_I2C_PORT,
        .sda_io_num                   = EEPROM_I2C_SDA,
        .scl_io_num                   = EEPROM_I2C_SCL,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed (SDA=%d SCL=%d) — EEPROM unavailable",
                 EEPROM_I2C_SDA, EEPROM_I2C_SCL);
        s_state = 0;
        return;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = EEPROM_I2C_ADDR,
        .scl_speed_hz    = EEPROM_CLOCK_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "I2C add-device 0x%02X failed", EEPROM_I2C_ADDR);
        s_state = 0;
        return;
    }

    // Presence — zero-length address probe; NACK = no chip on this board.
    if (i2c_master_probe(s_bus, EEPROM_I2C_ADDR, I2C_OP_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGI(TAG, "M24C32 absent (no ACK at 0x%02X)", EEPROM_I2C_ADDR);
        s_state = 0;
        return;
    }

    // Non-destructive RMW on the last byte.  Any stage failure → state 2,
    // always re-locking WE first.
    uint8_t orig = 0;
    if (!ee_read(EEPROM_TEST_ADDR, &orig, 1)) {
        ESP_LOGW(TAG, "M24C32 present but initial read failed");
        s_state = 2;
        return;
    }

    uint8_t test_val = (uint8_t)(orig ^ 0xA5);
    we_unlock();
    bool wrote = ee_write(EEPROM_TEST_ADDR, &test_val, 1);
    we_lock();
    if (!wrote) { ESP_LOGW(TAG, "M24C32 test write failed"); s_state = 2; return; }

    uint8_t readback = 0;
    if (!ee_read(EEPROM_TEST_ADDR, &readback, 1) || readback != test_val) {
        ESP_LOGW(TAG, "M24C32 readback mismatch (wrote 0x%02X got 0x%02X)",
                 test_val, readback);
        s_state = 2;
        return;
    }

    we_unlock();
    bool restored = ee_write(EEPROM_TEST_ADDR, &orig, 1);
    we_lock();
    if (!restored) { ESP_LOGW(TAG, "M24C32 restore write failed"); s_state = 2; return; }

    uint8_t verify = 0;
    if (!ee_read(EEPROM_TEST_ADDR, &verify, 1) || verify != orig) {
        ESP_LOGW(TAG, "M24C32 restore verify failed (want 0x%02X got 0x%02X)",
                 orig, verify);
        s_state = 2;
        return;
    }

    s_state = 1;
    ESP_LOGI(TAG, "M24C32 present + RMW OK (%u B, %u B/page, SDA=%d SCL=%d WE=%d)",
             EEPROM_SIZE_BYTES, EEPROM_PAGE_BYTES,
             EEPROM_I2C_SDA, EEPROM_I2C_SCL, EEPROM_I2C_WE);
}

uint8_t  eeprom_state(void)   { return s_state; }
uint16_t eeprom_size(void)    { return EEPROM_SIZE_BYTES; }
uint8_t  eeprom_page(void)    { return EEPROM_PAGE_BYTES; }
bool     eeprom_present(void) { return s_state != 0; }
