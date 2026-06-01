// SPDX-License-Identifier: GPL-2.0-or-later
//
// health.h — boot / crash / reboot counters persisted in NVS (namespace
// "cdc2net", alongside config + wifi creds) and surfaced in /api/status as the
// "health" block.  Lets you see across reboots how often the device faulted
// (panic / task-WDT / brownout) or was rebooted by the connectivity watchdog,
// vs clean/manual boots — without a serial console.
//
// Taxonomy:
//   boot_count   — every boot (incl. manual /api/reboot and OTA)
//   crash_count  — boots whose reset_reason was a fault: PANIC, TASK_WDT,
//                  INT_WDT, WDT, BROWNOUT
//   wdt_reboots  — reboots triggered by the *connectivity* watchdog
//                  (net_wdt_task → esp_restart, reset_reason=SW), counted
//                  explicitly so they don't hide among manual reboots

#ifndef CDC2NET_HEALTH_H
#define CDC2NET_HEALTH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call once after NVS is initialised (i.e. after net_init()).  Reads
// esp_reset_reason(), bumps boot_count, bumps crash_count if the last reset
// was a fault, and caches the values for the getters below.
void     health_boot_init(void);

// Call right before esp_restart() in the connectivity watchdog so the reboot
// is attributed to it (persisted, survives the reboot).
void     health_note_connectivity_reboot(void);

// Record whether this boot's main task subscribed to the Task-WDT.
void     health_set_wdt_subscribed(bool on);

uint32_t health_boot_count(void);
uint32_t health_crash_count(void);
uint32_t health_wdt_reboots(void);
bool     health_wdt_subscribed(void);

#ifdef __cplusplus
}
#endif

#endif // CDC2NET_HEALTH_H
