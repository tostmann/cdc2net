// SPDX-License-Identifier: GPL-2.0-or-later
//
// TULX32 build glue.  Only present in the tulx build (-DTULX_BUILD).
#pragma once

#ifdef TULX_BUILD
// Our entry in the TULX product manifest (OTA_CHECK_MANIFEST_URL).  The
// recovery installs from that list, so the update check reads the same entry
// and reports exactly what the recovery would install.
#define TULX_MANIFEST_FILE "firmware_cdc2net_tulx32.bin"

// Select the recovery system and restart.  Does not return on success.
void tulx_enter_recovery(void);

// Confirm this image if the bootloader put it on probation.  Call once, last,
// after start-up has actually reached the parts that can fail.
void tulx_confirm_running(void);
#endif
