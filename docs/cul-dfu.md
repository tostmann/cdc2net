# Reflashing the attached CUL (`/api/cul/dfu`)

USB-source builds can reflash the stick they are bridging. The legacy CUL
(ATmega32U4) normally appears on the host port as CDC-ACM and is passed through
to the TCP port; on command it jumps into its DFU bootloader and re-enumerates
as a different device — no longer the serial source, but a target that accepts a
new firmware. This is that path, driven from the bridge's own web API, with no
PC and no cable change.

Built only where `CONFIG_CDC2NET_SOURCE_USB` is set. UART-source missions (the
gateway among them) do not carry the code or the endpoints.

## Why an ESP32-S3 can do this at all

The Atmel DFU protocol needs **control transfers on endpoint 0 only** — no bulk
and no interrupt endpoints. That is the simplest case a USB host has to serve,
which is why a microcontroller host is enough. The protocol facts come from
[dfu-programmer](https://github.com/dfu-programmer/dfu-programmer)
(GPL-2.0-or-later), attributed in the source header.

## The sequence

1. `B01` goes to the stick over the normal bridge — it detaches.
2. It re-enumerates as `03EB:2FF4`, and this module takes over.
3. Erase, write, start application.
4. It comes back as CDC-ACM and the bridge takes over again.

> **Step 1 needs the stick's cooperation.** The jump into the bootloader is a
> command handled by the firmware already on the device. If that firmware is
> gone or does not implement it, there is no way in except a hand on the
> hardware. Recovering a bricked stick is not what this is for.

## Endpoints

**`POST /api/cul/dfu/enter`** — sends the jump command over the bridge.
The body may carry a different command; an empty body means `B01`. A trailing
newline from the caller is stripped and CRLF appended. Answers
`{"ok":true}`, or `400 source not ready` if no stick is attached.

**`GET /api/cul/dfu`** — whether a bootloader is on the bus, and how the last
run went:

```json
{"present":true,"busy":false,"vid":"03EB","pid":"2FF4",
 "written":15736,"eeprom":"intact",
 "msg":"fertig: 15736 Byte geschrieben, EEPROM unveraendert"}
```

`eeprom` is `unknown`, `intact` or `CHANGED` — the EEPROM is read before and
after a run and compared, because that is where culfw keeps its configuration.
`msg` carries the last message, including on failure; decide from `present`,
`busy` and `eeprom` rather than by matching its text.

**`POST /api/cul/dfu`** — the image, as the request body.

## The image must be raw binary

`cul_dfu_flash()` takes a **raw memory image starting at address 0**, the kind
`avr-objcopy -O binary` produces — *not* an Intel HEX file. Firmware for these
sticks is usually distributed as `.hex`, so this is the step people get wrong:

```
avr-objcopy -I ihex -O binary culfw.hex culfw.bin
curl --data-binary @culfw.bin http://cdc2net.local/api/cul/dfu
```

## What it refuses to do

Writes are capped at `CUL_DFU_APP_LIMIT` (`0x7000`) — everything below the
ATmega32U4's 4 KB bootloader. An image reaching past that is rejected rather
than written, because overwriting the bootloader leaves a stick that only
external hardware can revive.
