# CherryUSB — vendored copy

Upstream: [cherry-embedded/cherryusb](https://github.com/cherry-embedded/cherryusb),
version **1.6.1** as published on the ESP-IDF component registry
(`cherry-embedded/cherryusb`, Apache-2.0). `VERSION` and `LICENSE` are the
upstream files, untouched.

This used to be a registry dependency. It is vendored because it carries a local
fix (below) that a registry build cannot provide.

## What was removed

Upstream ships ~106 MB. Only what this project's build actually compiles was
kept (~2 MB):

| removed | why |
|---|---|
| `docs/` (82 MB) | documentation sources |
| `port/*` except `port/dwc2/` | other USB controller families |
| `demo/*` except `demo/usb_host.c` | examples; the kept file is referenced by `cherryusb.cmake` under `CONFIG_TEST_USBH_*` |
| `third_party/` | fatfs / FreeRTOS / lwip / NimBLE / Zephyr-BT copies, referenced only by the MSC and Bluetooth host classes |
| `tools/`, `tests/` | build tooling and unit tests |

`class/`, `core/`, `common/`, `osal/`, `platform/` and the top-level build files
are complete and unmodified apart from the patch.

**Consequence:** `CONFIG_CHERRYUSB_HOST_MSC_FATFS` and the Bluetooth host classes
will fail to configure — those reference `third_party/`. (Plain
`CONFIG_CHERRYUSB_HOST_MSC` still configures; its own sources are in-tree.)
Restore the directory from upstream 1.6.1 if they are ever needed.

## Local patch

`patches/0001-hub-boot-rescan.patch` (also applied to the sources here). It
fixes two defects in the external-hub driver, both observed on hardware
(ESP32-S31 + 4-port hub, devices attached behind it):

1. **`hub_int_complete_callback()` could stop the hub status poll for good.**
   The poll timer is one-shot and was only re-armed for `nbytes > 0` and for
   `-USB_ERR_NAK`; every other completion fell into an empty `else` branch. On
   the test rig those completions arrive regularly — 441 of them in ~16 minutes
   of idle operation, counted by `poll_err_count`, which does not record the
   code; an earlier build that logged each one saw `-USB_ERR_IO` and, far less
   often, `-USB_ERR_DT` (116 and 3 occurrences in the captures). Why the
   transfer fails at all is unexplained. The first such completion ended the
   poll — after which no port status change is ever collected again: no
   enumeration of devices behind the hub, and no hot-plug. Now always re-armed.
   The callback runs in the USB interrupt (`usb_glue_esp.c` installs
   `USBH_IRQHandler` via `esp_intr_alloc`, and `usb_hc_dwc2.c` calls
   `urb->complete` from it), so the error is only counted, never logged, in
   `poll_err_count`; the hub thread reports the counter.

2. **Devices already present when the hub attaches were never enumerated.**
   Enumeration was driven purely by the `C_CONNECTION` change bit. After an MCU
   reset the hub keeps its port state, so a pending change bit is only useful if
   the poll actually runs (see 1). `usbh_hub_connect()` now records every port
   that reports `CONNECTION` at attach time in `initial_scan_mask` and wakes the
   hub thread once; `usbh_hub_events()` runs those ports through the regular
   connect path (debounce → `PORT_RESET` → enumerate). One-shot, and the
   hot-plug path is unchanged.

Effect on the test rig: 7 consecutive warm resets enumerated the full chain
(before the fix: 2 of 4, alternating), and hub hot-plug re-binds the source
without a reset.

Not submitted upstream (yet). Both defects look generic — nothing in either is
specific to this SoC.

## Updating

Re-applying on a newer upstream: take the upstream tree, drop the directories in
the table above, then normalise line endings before patching — upstream ships
CRLF, the patch is LF, and GNU `patch` rejects the hunks over that ("different
line endings") with no flag to override:

    find . -name '*.[ch]' -exec sed -i 's/\r$//' {} +
    patch -p1 < patches/0001-hub-boot-rescan.patch

Verified against pristine 1.6.1: all hunks apply, and the patch also carries the
Apache-2.0 §4(b) change notices for the two files it touches. Then rebuild.
