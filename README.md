# CDC2NET

Transparent **USB-Host-CDC → network bridge** for the ESP32-S3 — a
ser2net-style gateway that puts an unmodified USB serial stick onto the
network (WiFi now, Ethernet later) **without touching the stick's firmware**.

Built for the **CUL / TUL / EUL** line:

- **legacy CUL** — native CDC-ACM (LUFA / ATmega32U4)
- **modern devices** — USB-Serial-JTAG of the ESP32-C3/C6 (CDC-ACM compliant)

The S3 is the USB host: it opens the stick's CDC-ACM interface and fans the
raw byte stream out to TCP clients (multi-client). No protocol framing, no
RFC2217 — a plain socket, so FHEM and friends talk to it exactly like a local
`CUL`, just over `host:port`.

![CDC2NET web interface](images/WebUI.png)

## Features

- **Transparent raw-TCP pipe** — default port `2329`, multiple clients fan-out.
- **USB host CDC-ACM** — opens any CDC-ACM stick (legacy CUL and the
  USB-Serial-JTAG of modern C3/C6 devices), shows its USB vendor/product
  strings.
- **WiFi onboarding** — Improv-Serial right after flashing, or a captive
  portal. Static-IP or DHCP, configurable TCP port, a connectivity watchdog.
- **Web UI** on port 80 — status, configuration, logs, and OTA.
- **OTA updates** — upload a `.bin` from the browser, or pull a release
  straight from the update server.
- **mDNS** — reachable as `cdc2net.local` (plus a unique `cdc2net-XXXX.local`);
  advertises `_http._tcp` for the web UI. The stream port is deliberately
  *not* advertised — what's on the host port is module-dependent.
- **One-click web flasher** — install over Web Serial from the browser.

## Why the S3

Only the ESP32-S2/S3/P4 have a USB-OTG **host** peripheral — a C3/C6 cannot be
the host. WiFi-only for now; a **W5500 Ethernet** option is planned (the
network layer is already link-abstracted for it).

## Install

### Web flasher (easiest)

Open **<https://install.busware.de/cdc2net/>** in a Chromium-based desktop
browser (Chrome / Edge / Opera / Brave — Web Serial isn't available in
Safari, Firefox, or on mobile), plug the S3 board into USB, and click
**Install**. The browser detects the chip and writes the factory image; WiFi
is handed over via Improv right after flashing.

> The flasher programs the **ESP32-S3 bridge board** — *not* the CUL/TUL/EUL
> stick. Your sticks keep their own firmware (culfw / a-culfw); CDC2NET only
> carries their serial line over the network.

### Manual flash

Grab `factory_cdc2net_esp32s3.bin` from the flasher directory and write it
with [esptool](https://github.com/espressif/esptool):

```sh
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 factory_cdc2net_esp32s3.bin
```

`firmware.bin` next to it is the app-only image used by the in-device OTA.

## Use

After flashing and WiFi onboarding, the device comes up as `cdc2net.local`.
Plug a stick into the host port and point your software at the raw-TCP port
(default `2329`). In FHEM:

```
define CUL_0 CUL cdc2net.local:2329 1234
```

Open `http://cdc2net.local/` for live status, WiFi/network/port
configuration, logs, and firmware updates.

## Build from source

PlatformIO with the ESP-IDF framework (IDF 5.5.x via `espressif32@6.13.0`):

```sh
cd firmware
pio run                       # build (pre-build hook bumps version + git-snapshots)
pio run -t upload             # flash over USB (set upload_port first)
pio device monitor            # 115200 baud, console on UART0
```

Release artifacts for the web flasher are produced by `firmware/scripts/release.sh`.

> **Build note (ESP-IDF #15079):** on the S3 the USB-OTG and USB-Serial-JTAG
> controllers share one USB PHY. Bringing up the WiFi PHY disables the USB PHY
> unless `CONFIG_ESP_PHY_ENABLE_USB=y` is set — which it is, in
> `firmware/sdkconfig.defaults`. Without it, the host stops enumerating once
> WiFi associates.

## Layout

```
firmware/            ESP-IDF (PlatformIO) project — the device firmware
firmware/web/        web UI sources (gzipped + embedded into the firmware)
firmware/scripts/    version bump + web-asset embed + release packaging
webflasher/          ESP Web Tools manifest + landing page (release artifacts)
images/              screenshots / assets for this README
docs/                design notes and roadmap
```

## License

GPL-2.0-or-later (see the SPDX headers in the sources).

---

CDC2NET · © 2026 Dirk Tostmann · <https://github.com/tostmann/cdc2net> ·
hosted at [busware.de](https://busware.de)
