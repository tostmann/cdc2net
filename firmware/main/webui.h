// SPDX-License-Identifier: GPL-2.0-or-later
//
// webui.h — HTTP status/OTA server for CDC2NET (M4 part 1).
//
// Endpoints:
//   GET  /                  embedded single-page dashboard (index.html.gz)
//   GET  /api/status        JSON: fw, net, usb-source, bridge, tcp, sys
//   GET  /api/log?since=N   JSON: ring-buffer log lines since seq N
//   POST /api/reboot        200 + restart in 500 ms
//   POST /api/ota           raw firmware.bin body -> passive OTA slot + boot
//   GET/POST /api/update/check   online manifest version check (cached/refresh)
//   + captive-portal probe redirects (Android/Apple/Windows)
//
// WiFi config / scan / auth / diagnostics come in M4 part 2.

#ifndef CDC2NET_WEBUI_H
#define CDC2NET_WEBUI_H

#include "esp_err.h"

esp_err_t webui_init(uint16_t port);

#endif // CDC2NET_WEBUI_H
