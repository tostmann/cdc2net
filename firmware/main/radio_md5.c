// SPDX-License-Identifier: GPL-2.0-or-later
//
// radio_md5.c — MD5 helper for radio_flash.c, deliberately in its own
// translation unit.
//
// esp-serial-flasher's public esp_loader.h includes md5_ctx.h, which defines
// `struct MD5Context`.  IDF's mbedtls port reaches the identically named struct
// through mbedtls/md5.h -> port/include/md/esp_md.h -> esp_rom_md5.h.  Both in
// one .c file is a redefinition error, and neither side can be avoided:
// radio_flash.c needs the loader API, and the flasher's own MD5 functions are
// private (md5_hash.h).  So the hash lives here, where esp_loader.h never is.

#include <stddef.h>
#include <stdint.h>

#include "mbedtls/md5.h"

// Output is the 32-char LOWERCASE HEX form plus NUL — not the 16 raw bytes.
// esp_loader_flash_verify_known_md5() takes a `const uint8_t *`, which reads
// like a raw digest, but compares it against what the ROM loader returns: an
// ASCII hex string (MD5_SIZE_ROM = 32).  The library hexifies its own digest
// before calling that very function (esp_loader.c, flash_finish).  Passing raw
// bytes therefore never matches — bench-observed as an endless re-flash loop.
void radio_md5_hex(const uint8_t *data, size_t len, char out[33])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t raw[16];
    mbedtls_md5(data, len, raw);
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = hexd[raw[i] >> 4];
        out[2 * i + 1] = hexd[raw[i] & 0x0F];
    }
    out[32] = '\0';
}
