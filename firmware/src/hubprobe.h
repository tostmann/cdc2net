// SPDX-License-Identifier: GPL-2.0-or-later
//
// hubprobe.h — THROWAWAY USB-host channel-budget probe entry point.
// Only compiled/linked when -D CDC2NET_HUBPROBE is set (env:hubprobe).
#pragma once

void hubprobe_run(void);
