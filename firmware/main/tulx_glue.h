// SPDX-License-Identifier: GPL-2.0-or-later
//
// TULX32 build glue.  Only present in the tulx-c6 build (-DTULX_BUILD).
#pragma once

#ifdef TULX_BUILD
// Select the recovery system and restart.  Does not return on success.
void tulx_enter_recovery(void);

// Confirm this image if the bootloader put it on probation.  Call once, last,
// after start-up has actually reached the parts that can fail.
void tulx_confirm_running(void);
#endif
