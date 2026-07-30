// SPDX-License-Identifier: GPL-2.0-or-later
//
// eeprom_m24c32.h — M24C32 (32-Kbit I2C serial EEPROM) presence + geometry
// for the EUL/TUL C6 generation.  Compiled only when CONFIG_CDC2NET_EEPROM_M24C32
// is set (the C6 EUL carrier; the shared bridge PCB leaves the pads unpopulated).
//
// IDF port of EULFW32's Arduino-Wire M24C32 self-test (src/main.cpp).  The boot
// probe is a NON-DESTRUCTIVE read-modify-write on the last byte, gated by the
// board's dedicated WE (write-control) pin — so a present chip is verified
// without poisoning its contents.

#ifndef CDC2NET_EEPROM_M24C32_H
#define CDC2NET_EEPROM_M24C32_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot-time: init the I2C master + WE GPIO, probe 0x50, and run the
// non-destructive RMW self-test.  Idempotent enough for a single boot call;
// leaves the I2C bus initialised + WE locked.  Records the result in a static
// readable via eeprom_state().  Never asserts — a missing/broken chip just
// yields state 0/2.
void eeprom_init_and_test(void);

// 0 = absent (no ACK at 0x50) / 1 = present + RMW OK / 2 = present, RMW failed.
// Mirrors EULFW32's g_eeprom_state taxonomy 1:1.
uint8_t eeprom_state(void);

// Geometry (constant for the M24C32): total bytes / page size in bytes.
uint16_t eeprom_size(void);
uint8_t  eeprom_page(void);

// Convenience: state != 0 (chip answered on the bus).
bool eeprom_present(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_EEPROM_M24C32_H
