# ESP Zigbee-Gateway (`zbgw`)

Design notes for the gateway variant: Espressif's **ESP Thread Border Router /
Zigbee Gateway board** (ESP32-S3 host + ESP32-H2 radio) turned into a
network-attached Zigbee coordinator.

Build environment: `env:zbgw-s3`. Flasher channel:
<https://install.busware.de/cdc2net/zbgw/>.

## Architecture

The H2 runs the full **ZBOSS NCP** firmware
([esp-coordinator](https://github.com/tostmann/esp-coordinator)) and reaches the
S3 over the board's inter-chip UART. The S3 runs CDC2NET with the **UART
source** instead of the USB host (`CONFIG_CDC2NET_SOURCE_UART=y`,
`CONFIG_CDC2NET_SOURCE_USB=n`) and is a byte-transparent UART → TCP bridge on
port `2329`.

Nothing on the host parses Zigbee: the radio speaks ZBOSS NCP, not Spinel, so
the S3 never has to implement a stack. Zigbee2MQTT connects as it would to a USB
stick:

```yaml
serial:
  adapter: zboss
  port: tcp://<gateway-ip>:2329
```

The `zboss` adapter is marked experimental upstream — see the project README
before putting a production network on it.

## The radio firmware ships inside the host image

The H2's firmware lives in the S3's own flash, in a dedicated `radio_fw`
partition, and is written to the H2 over the inter-chip UART plus its reset and
boot straps (via `esp-serial-flasher`). Espressif carries its own `ot_rcp` the
same way; the difference is what gets written.

**Sync policy** (`main/radio_flash.c`, contract in `radio_flash.h`): the radio's
running `esp_app_desc` is read back over the download protocol and compared
against the staged image. A mismatched project name — a factory-fresh board
still running `ot_rcp` — or a different version means *write*; identical means
*leave it alone*. The step runs before the UART source claims the port, owns the
port for the duration, and releases it again. It is never fatal: if the download
protocol cannot be entered, the bridge still comes up.

Two consequences worth knowing:

- A **factory flash of the S3 takes over the whole board** (first boot takes
  roughly half a minute longer while the radio is rewritten and MD5-verified).
- An **app-only OTA updates the host and leaves a matching radio untouched** —
  the `radio_fw` partition sits outside both OTA slots.

The partition is appended after the existing layout, so a device flashed before
it existed still takes an OTA.

### Updating the radio over the network

`POST /api/radio` stages a new radio image without touching USB. The body is the
**merged factory image** of the radio SoC (bootloader at 0, partition table,
otadata, app at `0x20000`) — exactly what `radio_flash.c` consumes. The upload
only rewrites the `radio_fw` partition and reboots; the actual inter-chip flash
happens in `radio_flash_sync()` on the way up, which re-validates and
MD5-compares as always. Staging an image the radio already runs is therefore a
no-op, and a new version costs the usual ~25 s reflash.

Guards, in order:

1. a `radio_fw` partition must exist — gateway boards only, `404` elsewhere;
2. `content_len` inside `[app-descriptor minimum, partition size]`;
3. first byte `0xE9`, the radio bootloader's image magic;
4. **chip-id fence**: header offset 12 must match the chip id of the image
   currently staged, when there is one. That is a self-consistent cross-chip
   check without a target table — the board knows which radio it carries because
   the previous image was for it. A blank partition skips the check (first
   stage). Without it, a wrong-chip image would put the radio into a boot loop
   that the self-healing sync would re-flash forever;
5. after the write, the radio's app descriptor is read back from flash and its
   magic word verified. On failure the partition is left invalid and
   `radio_flash_sync()` refuses to touch the radio, which keeps running its
   current firmware — no destructive half-state.

**Commit-marker scheme.** Sector 0 carries the `0xE9` magic, so it is held in RAM
and written *last*, after the whole body arrived and the descriptor verified. A
brownout or dropped connection mid-upload then leaves the first byte at `0xFF`
and the stage is refused. Without it, a cut past the app descriptor would leave
a truncated-but-valid-looking image that the self-healing sync would flash onto
the radio forever. The whole partition is erased rather than just `content_len`,
because the image size is derived by trimming trailing `0xFF` — leftovers of a
longer previous image would otherwise be read as part of the new one.

| Partition  | Offset     | Size    | Purpose                          |
|------------|------------|---------|----------------------------------|
| `ota_0`    | `0x010000` | 3 MB    | app slot                         |
| `ota_1`    | `0x310000` | 3 MB    | app slot                         |
| `coredump` | `0x610000` | 64 KB   | ELF written on panic             |
| `radio_fw` | `0x620000` | 768 KB  | staged ESP32-H2 image            |

### Why not just flash the H2 directly

Espressif's factory S3 firmware reflashes the H2 back to `ot_rcp` when it
detects the radio failing to come up as a Spinel RCP — a documented recovery
feature. Flashing coordinator firmware onto the H2 alone therefore does not
stick; the S3 has to be taken over as well, which is what this image does.

## Board wiring

From `env:zbgw-s3` in `firmware/platformio.ini` and the defaults in
`main/radio_flash.c`:

| Signal                | S3 pin | Notes                                     |
|-----------------------|--------|-------------------------------------------|
| Inter-chip UART TX    | 18     | → H2 RX, 115200 baud in bridge operation  |
| Inter-chip UART RX    | 17     | ← H2 TX                                   |
| Radio reset strap     | 7      | → H2 `EN`                                 |
| Radio boot strap      | 8      | → H2 `GPIO9` (download-mode select)       |
| W5500 `SCLK`          | 21     | Ethernet sub-board                        |
| W5500 `MOSI`          | 45     |                                           |
| W5500 `MISO`          | 38     |                                           |
| W5500 `CS`            | 41     |                                           |
| W5500 `INT`           | 39     |                                           |
| W5500 `RST`           | 40     |                                           |

Radio flashing syncs at 115200 and transfers at 460800. Both straps are returned
to inputs afterwards, so the host never holds the radio in reset.

This wiring is specific to the gateway board. The generic ESP32-H2 image from
the stick flasher uses different UART pins and will not talk to this S3.

**Flash the socket marked `USB2`** — that is the S3. The board's other USB-C
(`USB1`) goes to the H2 and cannot write this image; a browser reporting an
ESP32-H2 after port selection means the wrong socket.

## Console and onboarding

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`: this board's USB-C goes straight to the
S3's native USB-Serial/JTAG — there is no CH34x bridge as on the YD-ESP32-S3
dev board, and UART0 only reaches a pin header. Improv-Serial picks its
transport from that same option, so with UART0 as the console it would listen on
a port no user can reach, leaving the captive AP as the only way in.

## Network uplink

Wired Ethernet comes from the optional W5500 sub-board, driven by
`main/net_eth.c` (in-core `esp_eth` W5500 driver) behind
`CONFIG_CDC2NET_ETH_W5500`. A link is picked up by DHCP at boot and takes
precedence; the captive AP then stays off, since the web UI is reachable over
the network anyway. An absent W5500 makes `esp_eth_driver_install()` fail — it
reads the chip version — which is logged and skipped, leaving WiFi unaffected.

> **Known limitation.** If the gateway boots with no cable *and* no WiFi
> credentials, it opens its configuration access point after about two minutes.
> Plugging the network cable in after that point may leave the Ethernet link
> stuck; a restart brings it up normally. Booting with the cable already
> connected, or plugging it in before the access point appears, is unaffected.

## OTA channel separation

The mission's manifest and firmware URLs are baked into the image via
`build_flags`, and `scripts/release.sh` asserts both URLs are present in what it
just built.

This is not cosmetic. Every variant shares `project(cdc2net)`, so the OTA pull
path's project-name guard would *not* stop a plain CDC2NET S3 image from
installing itself over the gateway build — which would cost the ZB_NCP profile,
the W5500 pin map and `radio_flash`. The chip-id guard does not help either,
since both are S3. **The feed URL is the product identity.**

## Built for 8 MB flash

Espressif's documentation for the board says 8 MB flash, and 8 MB boards are in
the field (`tostmann/esp-coordinator#11`: bootloop with `Detected size(8192k)
smaller than the size in the binary image header(16384k)`). The development
board behind this firmware carries 16 MB.

The bootloader adopts `spi_size` from the image header as the ROM flash size,
and `esp_flash_init_default_chip()` asserts when the detected chip is *smaller*
than the header — larger is unproblematic. An 8 MB image therefore runs on both
populations, and the layout already fits: the last partition ends at `0x6E0000`
(≈ 6.9 MiB). Hence `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`, overriding the 16 MB
default the other S3 variants keep using.

## PSRAM: present and usable, deliberately off

Hardware-verified board facts (they differ from the published board
description, which lists 8 MB flash / 2 MB PSRAM):

- eFuse: `PSRAM_CAP=8M`, `PSRAM_VENDOR=AP_3v3`, `PSRAM_TEMP=85C`
- A test build with octal PSRAM enabled came up cleanly — vendor `0x0d` (AP),
  density 64 Mbit, good-die pass, 8 MB added to the heap allocator; free heap
  8,468,148 B instead of ~141 KB, with bridge and Zigbee round-trip unremarkable
  in a smoke test.

It stays **off** in the product image because nothing currently profits: the
bridge buffers are small, and PSRAM changes memory layout and cache behaviour
next to WiFi and SPI DMA — risk without benefit. Turn it on when there is a
reason (large replay/fan-out buffers), and then validate with a soak test rather
than a smoke test.

> **Trap when enabling it:** PlatformIO does *not* regenerate
> `firmware/sdkconfig.<env>` when only the defaults fragment is extended —
> delete `firmware/sdkconfig.zbgw-s3` first, or the options never reach the
> build.
