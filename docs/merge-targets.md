# CDC2NET / EUL / TUL / SER2THREAD — Build-Target-Matrix (ein ESP-IDF-Tree)

**Entscheidung (Dirk, 2026-06-10):** Merge von CDC2NET + EULFW32 als **EIN ESP-IDF-Baum**,
pro Build gegated über Kconfig-`choice` + `sdkconfig.defaults.<chip>`-Overlay + CMake-`if()`
auf SRCS/REQUIRES. EULFW32 (Arduino) bleibt separates Repo; hier wandert dessen
*Funktionalität* als IDF-Neuimplementierung ein, EULFW32 dient als Blueprint.

Framework: PlatformIO `espressif32@6.13.0` = **IDF 5.5.3** (verifiziert).

Diese Datei ist die kanonische Target-Liste. Alle Zellen gegen Primärquellen
(Espressif-Datenblätter, IDF-Component-Registry, `dependencies.lock`) und gegen den
echten Code-Tree geprüft; spekulative Zellen explizit als *aspirational* / *blocked*
markiert.

## 0. Verifizierte harte Constraints (Silizium — kein `#define` löscht sie)

- **USB-Host** (`cdc_acm_host_open` + VCP-Class-Driver) braucht **USB-OTG** → nur **S2/S3/P4**.
  C3/C5/C6 haben **nur** fixed-function USB-Serial-JTAG (CDC-*Device*), nie Host.
  (`dependencies.lock`: `usb_host_cdc_acm` targets = `esp32s2/s3/p4/h4/s31`.)
- **S3** interne USB-PHY = **Host XOR Device**. Im Host-Modus muss Konsole auf **UART0**;
  `CONFIG_ESP_PHY_ENABLE_USB=y` ist **load-bearing** (#15079: WiFi-Init disabled sonst die
  USB-PHY → Host-Enumeration stirbt sobald WiFi assoziiert).
- **802.15.4 / on-die-Thread**: nur **C6 / C5 / H2** (`SOC_IEEE802154_SUPPORTED`).
  **C3 und S3 haben kein 15.4-Radio** → on-die-Thread unmöglich; S3 kann Thread nur als
  *Host eines externen RCP*.
- **C5** ist auf `espressif32@6.13.0` **nicht baubar**: kein `esp32-c5-devkitc-1.json`
  (404 am v6.13.0-Tag). C5-Silizium ist in IDF ab **5.5.2** nutzbar, „full support" erst
  **v6.0**. → C5-Zeilen sind **blocked**, nicht „roadmap-locker".
- **CC1200 (CUL32)** = SPI-Radio, **kein** serielles Downlink → künftige `source_spi.c`,
  keine `source_uart`-Zeile.
- **S31 = USB-HS-Host (16 Kanäle), aber NUR mit CherryUSB:** der S31-OTG hat ausschließlich
  den UTMI-HS-PHY → Root-Port läuft HS, FS-Sticks hinter einem Hub brauchen Split-Transactions
  (TT). esp-usb (1.4.x) implementiert keinen TT und disabled solche Ports; CherryUSB ≥1.6 kann
  Splits (HW-bewiesen: Hub + 3 FS-Geräte gleichzeitig, Kanal-Vergabe dynamisch pro URB).
  `esp32s31` ist Preview-Target auf IDF master → **kein PlatformIO-Env**; Build aus demselben
  Tree via `idf.py --preview set-target esp32s31 && idf.py build` (Overlay
  `sdkconfig.defaults.esp32s31` macht das zero-arg; App-Sourcen liegen dafür in `main/`).
- **W5500-Ethernet = linienweite First-Class-Option (nicht nur S3-M5):** alle busware TUL/EUL
  der **neuen C6-Generation** tragen einen **FPC-Header für nachträgliches W5500**. ⇒ ETH gilt
  für die C6-Stick-Linie genauso wie für die S3-Bridge; jede C6-Stick-Mission hat eine
  `-eth`-Variante. `net.c` ist link-abstrahiert (STA‖ETH); der `esp_eth`/W5500-SPI-Treiber ist
  linienweit gemeinsam, nur die Pin-Defs sind per-Board. **W5500-SPI-Pins auf C6 aus
  busware-FPC-Schematic / EULFW32** (EULFW32 fuhr C6-W5500 bereits — CS=GPIO17, teilt UART0-TX,
  im USB-Serial-JTAG-Konsolenmodus frei) — **nicht raten**. C3-legacy: kein FPC.

## 1. Naming + Defaults

- **Env-id:** `<mission>-<chip>[-<link>]`, lowercase. `mission ∈ {cdc2net,eul,tul,ser2thread}`,
  `chip ∈ {s3,c6,c3}` (c5 blocked), `link ∈ {wifi,eth}`.
- **TCP-Port:** **2329** für den ganzen IDF-Tree (CDC2NET-Default, fw 0.1.48). EULFW32s `2323`
  bleibt im Arduino-Tree. Port ist ein C-`#define` (`config.h:18`) + NVS-Override
  (`config.c`), **nicht** Kconfig — per-Mission-Divergenz bräuchte Promotion zu Kconfig/`-D`.

## 2. Die Matrix (eine Zeile pro physisch möglichem Target)

| env | board-id | chip | mission | source | USB-Rolle | net | WG | RFC2217 | partition | status |
|---|---|---|---|---|---|---|---|---|---|---|
| `cdc2net-s3-wifi` | `esp32-s3-devkitc-1` | S3 | USB-Host-Bridge: ext. CUL/VCP-Stick → raw TCP | `source_usb` +3 VCP | **HOST** (OTG) | WiFi+SoftAP | backlog | **ja (shipped)** | `partitions_ota.csv` 16M/3M | **SHIPPED 0.1.48** |
| `cdc2net-s3-eth` | `esp32-s3-devkitc-1` | S3 | + W5500 wired uplink | `source_usb` +3 VCP | HOST (OTG) | **ETH**+WiFi | backlog | ja | `partitions_ota.csv` | M5 geplant |
| *(kein env — idf.py)* | *IDF master `--preview`* | **S31** | USB-**HS**-Host-Bridge, 16 Kanäle, Multi-Stick-Ziel (mehrere Sticks hinter Hub) | `source_usb_cherry` (Pflicht, TT) | **HOST** (OTG-HS) | WiFi+SoftAP | backlog | ja | `partitions_ota.csv` 16M/3M | **✅ HW-VERIFIZIERT 2026-07-22/23** (Boot/CUL/WiFi/Improv/OTA/RFC2217/FHEM-RX/10-h-Soak; Hub-Rig offen) |
| `eul-c6` (`-eth`) | `esp32-c6-devkitc-1` | C6 | EnOcean-Stick (TCM515) — **byte-transparent** (kein on-device ESP3-Framer; FHEM dekodiert end-to-end) | `source_uart` ✅ | device-only | WiFi+SoftAP **+W5500** | backlog | ja† | `partitions_eul_4m.csv` (**4M** modul!) | **✅ HW-VERIFIZIERT 2026-06-10 (W5500-DHCP + TCM515-BaseID FF:EE:52:00)** |
| `eul-c3-wifi` | `seeed_xiao_esp32c3` | C3 | EnOcean-Stick, low-flash | `source_uart` (neu) | device-only | WiFi+SoftAP | backlog | optional† | `partitions_c3_4m.csv` (neu, 1.856M) | roadmap |
| `tul-c6-wifi` | `esp32-c6-devkitc-1` | C6 | KNX-Stick (NCN5130) | `source_uart` + KNX-Decoder | device-only | WiFi+SoftAP | backlog | optional† | `partitions_c6.csv` | **aspirational — keine Impl** |
| `ser2thread-rcp-s3-wifi` | `esp32-s3-devkitc-1` | S3 | ext. C6/H2-RCP (Spinel) als serielle Source → TCP | `source_usb`/`source_uart` **byte-transparent** | HOST od. UART | WiFi+SoftAP | backlog | n/a | `partitions_ota.csv` | roadmap — **billigster Thread, reused Naht, kein neues Framework** |
| `ser2thread-c6-wifi` | `esp32-c6-devkitc-1` | C6 | on-die Thread-Node → TCP | `source_thread` (neu) | device-only | WiFi **+Thread (coex)** | backlog | n/a | `partitions_c6.csv` | roadmap — ⚠ single-RF, Paketverlust unter WiFi-Last |
| `ser2thread-c5-wifi` | *custom JSON nötig* | C5 | on-die Thread, dual-band WiFi | `source_thread` | device-only | WiFi(2.4/5G)+Thread | backlog | n/a | — | **BLOCKED** (kein board-id @6.13.0; C5-15.4 jung) |

† RFC2217/per-port-serial ist **nur auf echtem UART-Downlink** (EUL/TUL) sinnvoll — Baud
trifft den Draht. Codec (`serialcfg.c`) ist source-agnostisch; der *Apply*-Pfad hängt heute
an `source_usb_apply_line_coding` und muss erst aufs vtable (§3.5). Auf Thread/RCP-Zeilen
n/a (Spinel besitzt die Link-Parameter).

‡ **W5500 via FPC-Header** gilt für die **C6-Stick-Generation** → `eul-c6-eth` / `tul-c6-eth` /
`ser2thread-c6-eth` sind legitime Geschwister der `-wifi`-Zeilen (net-Spalte oben zeigt nur den
WiFi-Default; ETH ist additiv über `net.c`, STA‖ETH). C6-W5500-Pins aus busware-FPC-Schematic /
EULFW32 (CS=GPIO17). C3-legacy hat **keinen** FPC.

### N/A — Silizium verbietet (selbst-dokumentierend)

| Kombo | Grund |
|---|---|
| `cdc2net-c6/c3/c5` (USB-Host auf C-Chip) | kein USB-OTG, nur USB-Serial-JTAG-Device. |
| `ser2thread-c3-*` (on-die Thread C3) | C3 hat kein 802.15.4-Radio. |
| `ser2thread-s3-*` (on-die Thread S3) | S3 hat kein 802.15.4 → nur Host-of-RCP (= `ser2thread-rcp-s3`). |
| `eul-s3` Host **und** UART gleichzeitig | S3-PHY = Host XOR Device; UART-EUL auf S3 möglich aber sinnlos (nimm C-Chip). |
| CUL32 (CC1200) als UART-Zeile | CC1200 = SPI → künftige `source_spi.c`, keine UART-Zeile. |

## 3. Build-Mechanik (Minimum, damit die Matrix baut)

### 3.1 Source-Wahl — Kconfig `choice` (`Kconfig.projbuild`)
`CDC2NET_SOURCE_{USB,UART,THREAD,RCP}`; `main.c:54-55` (einzige Konstruktionsstelle,
`s_usb=source_usb_init(); bridge_attach_source(...)`) wird `#if`-gegated; globalen
`s_usb` → `s_src` umbenennen (betrifft auch `main.c:100 source_describe`).

### 3.2 Component-Gating — **am meisten unterschätzt**
`main/CMakeLists.txt` listet heute hart in `REQUIRES`: `usb usb_host_cdc_acm
usb_host_ftdi_vcp usb_host_ch34x_vcp usb_host_cp210x_vcp`. Die **bauen nicht auf C3/C6**.
→ CMake-`if(CONFIG_CDC2NET_SOURCE_USB)` um SRCS **und** REQUIRES.
**Achtung (über main/CMakeLists hinaus):** die 3 VCP-Components unter `firmware/components/`
werden vom IDF-Component-Manager **auto-gescannt** und laufen ihren eigenen configure-Schritt
**unabhängig** von REQUIRES → sie müssen **selbst target-guarden** (supported_targets in ihrer
`idf_component.yml`/CMakeLists), sonst failt ein C6-Build an *ihrem* configure-step. Nur
main/CMakeLists gaten ist **notwendig, nicht hinreichend**. (Hängt mit der Regel zusammen,
kein `EXTRA_COMPONENT_DIRS` zu setzen.)

### 3.3 `sdkconfig.defaults.<chip>`-Overlay
- **S3:** `CONFIG_ESP_PHY_ENABLE_USB=y` (load-bearing) + `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`.
- **C6/C3:** `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (Board-Konvention — IDFs *literaler*
  Kconfig-Default ist UART0 auf **allen** Chips), USB-Host aus.
- **Thread (C6/C5):** `CONFIG_OPENTHREAD_ENABLED=y` + `CONFIG_IEEE802154_ENABLED=y` +
  `CONFIG_OPENTHREAD_RADIO_NATIVE=y` + `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`.
- **RCP-Host (S3, nur falls S3-als-OT-host statt purem Tunnel):** `CONFIG_OPENTHREAD_ENABLED=y`
  + `CONFIG_OPENTHREAD_RADIO_SPINEL_UART=y`, **kein** `IEEE802154_ENABLED`.

### 3.4 Partition / Flash
- **S3/16M:** `partitions_ota.csv` (2×3M-Slots) — heute.
- **C6:** `esp32-c6-devkitc-1` gibt es in **4M und 8M** → Slot-Größe hängt vom realen Modul ab.
  8M → ~3M-Slots; **4M → gleiche Overflow-Falle wie C3**, dann 1.856M-Slot-Layout. Flash des
  Produktionsmoduls **vor** dem Einfrieren bestätigen.
- **C3/4M:** neues `partitions_c3_4m.csv` (EULFW32-Stil, 1.856M-Slots + coredump).

### 3.5 Der EINE geteilte Prerequisite — `webui.c` (+ `main.c:95`) aufs vtable
`webui.c` ist **nicht** source-agnostisch: `#include "source_usb.h"` (`:11`) +
`source_usb_get_stats` (`:157`), `source_usb_get_serial_info` (`:476/:535/:571`),
`source_usb_apply_line_coding` (`:538/:573`). **Zweite Stelle:** `main.c:95` ruft ebenfalls
`source_usb_get_stats`. → vor jeder non-USB-Source:
- `_get_serial_info`/`_apply_line_coding` → `source_get/set_line_coding` (existieren als
  Inlines, `source.h:109/:118`);
- `_get_stats` → neue generische `source_get_stats`-Op in `struct source_ops` (`source.h:46`),
  **beide** Call-Sites (`webui.c:157` + `main.c:95`) umstellen.

**WebUI-Karten-Set an EULFW32 angeglichen (2026-06-10):** die CDC2NET-Dashboard hat jetzt
dieselben **„Device"-** + **„EEPROM"**-Kacheln wie `EULFW32/src/status_page.h` (`t-device`,
`cat-eeprom`). „Device" = Hostname/Chip+Rev/MAC/Firmware/Uptime/Heap/Reset (Identität aus
„System" herausgezogen → „System" trägt nur noch Health/Supervisor + WDT-Config + Reboot, exakt
EULFW32s Device↔Supervisor-Split). Backend in `webui.c h_status`: neuer `features`-Block +
`sys.chip`/`chip_rev`/`mac` (via `esp_chip_info` + `esp_read_mac` + `CONFIG_IDF_TARGET`).
- **Repräsentations-Entscheidung (load-bearing fürs spätere Shared-UI):** CDC2NET sendet `chip`
  als **String** (`CONFIG_IDF_TARGET` = „esp32s3") und `reset_reason` als **aufgelösten String**.
  EULFW32 sendet beide **numerisch** + JS-Map (`CHIP_MODELS`/`RESET_REASONS`). **Kanonisch fürs
  gemergte IDF-Tree ist die String-Form** — sie ist self-consistent (ein Producer, ein Consumer),
  braucht kein Per-Build-`#define`, keine Map die vom Enum driftet, und ist im rohen `/api/status`
  direkt lesbar (Support). Beim Zusammenführen der UIs migriert **EULFW32 auf die String-Form**,
  nicht umgekehrt. (Hard-Requirement bleibt nur: der eine Producer und der eine Consumer stimmen
  überein — heute der Fall.)
- **EEPROM-Kachel: IDF-M24C32-Backend GEBAUT + HW-VERIFIZIERT (2026-06-10).** `src/eeprom_m24c32.{c,h}`
  ist der Port von EULFW32s Arduino-`Wire`-RMW-Probe auf `esp_driver_i2c` (neue I2C-Master-API):
  Boot-Probe @0x50 → zerstörungsfreier RMW auf dem letzten Byte (read orig → write orig^0xA5 →
  verify → restore → verify) → state 0/1/2, plus WE-Pin-Gating (HIGH=lock). Gegated via
  `CONFIG_CDC2NET_EEPROM_M24C32` (Kconfig.projbuild + `if()` in main/CMakeLists → `esp_driver_i2c`),
  **default n**, in `sdkconfig.defaults.esp32c6` auf `y`. webui.c emittiert `features.eeprom` +
  `eeprom{state,size,page}` (Block immer da, Nullen wenn nicht compiliert → uniforme Shape; JS liest
  ihn nur bei `features.eeprom===true`). Pins = EULFW32-C6-Map SDA=22/SCL=23/WE=15 (kollidieren NICHT
  mit C6-W5500 {19,20,21,17,0} / TCM-UART {4,5,3,2}), via `-D` überschreibbar.
  **Bench (eul-c6, MAC 8C:FD:…:2BF8):** `M24C32 present + RMW OK (4096 B, 32 B/page)`, `/api/status`
  `eeprom:{state:1,size:4096,page:32}`. **Bridge (S3):** Gate aus → `features.eeprom=false`, Block
  Nullen, Kachel versteckt, kein Regress. **Fallstrick beim Port:** ACK-Polling via
  `i2c_master_probe` im Write-Pfad schlug fehl (state=2 „test write failed" trotz vorhandenem Chip)
  → ersetzt durch **fixen tW-Delay (`vTaskDelay`) + Readback als Completion-Check** (der Readback
  verifiziert ohnehin); seither RMW OK. Die geteilte Bridge-PCB lässt die EEPROM-Pads unbestückt
  (RFNETHM `decisions.md`) — auf der Bridge bleibt die Kachel dauerhaft aus.

### 3.6 OTA-Cross-Flash — chip_id ist IDF-gated; project_name + Release-Truthfulness nachgerüstet (2026-06-10)
**Verifiziert an der IDF-5.5.3-Quelle (frühere Notiz „nur magic+secure_version → Brick" war FALSCH):**
`esp_https_ota_perform()` ruft `esp_ota_verify_chip_id()` im BEGIN-State **vor** dem ersten
Flash-Write (`esp_https_ota.c:719` → `:644`); chip_id-Mismatch → `ESP_ERR_INVALID_VERSION`,
Boot-Partition bleibt unangetastet. Die chip_ids sind alle distinkt (S3=0x9, C3=0x5, C6=0xD).
⇒ Ein falsch-**Chip**-Image (C6↔S3 etc.) kann den **In-Device-OTA-Pull nicht bricken** — es wird
abgelehnt, das Gerät läuft mit alter FW weiter. Auch eine höhere `secure_version` hilft dem
falschen Chip nicht (chip_id wird zuerst geprüft).

**Was IDF NICHT prüft:** `project_name` (andere App für *denselben* Chip). **Nachgerüstet**
(`ota_check.c`, fw ≥0.1.82): granulare API `begin→get_img_desc→project_name-Reject→perform→
finish`; chip/rev-Mismatch wird als klarer, distinkter Fehler im `/api/status`-install-Feld
gesurfaced statt als nackter Code.

**Der echte Brick-Vektor im Merge saß in `release.sh`** — es hardcodete `esp32s3` /
„ESP32-S3" an drei Stellen. Ein damit geschnittenes Non-S3-Release hätte ein Manifest erzeugt,
das über `chipFamily` **lügt** — und esp-web-tools gated den Webflash genau an diesem Feld.
**Gefixt:** chip / chipFamily / Flash-Settings / **alle Merge-Offsets** werden pro Build aus
`flasher_args.json` (+ otadata aus der echten `partitions.bin`) abgeleitet → Manifest immer
wahrheitsgemäß, korrekte Offsets für 16M-S3 *und* 4M-C3/C6; zusätzlich ein
`firmware.bin`-chip_id-Assert im Release-Build.

**Noch offen (B — kein Brick, sondern Funktion):** der In-Device-OTA-*Pull* nutzt eine
S3-only Compile-Konstante (`ota_check.h` → `install.busware.de/cdc2net/firmware.bin|manifest.json`).
Multi-Target-OTA-Pull braucht per-Chip-URL-Auswahl + eine **Server-Layout-Entscheidung**
(per-Chip-Subdir vs. chip-suffigierte Dateinamen unter `install.busware.de`), bevor eine zweite
Mission OTA-Updates *bekommt*. Bis dahin: Non-S3-Targets werden per **Webflasher** geflasht
(jetzt sicher), nicht per OTA-Pull. Brick-Story ist damit zu; Funktions-Story wartet auf (B).

## 4. Build-Reihenfolge (kleinster Pfad von heute S3-only zur Matrix)

1. **`cdc2net-s3-wifi` bleibt Referenz** (shipped, 0.1.48) — Verhalten nicht anfassen.
2. **Refactor-Pass (ein PR, keine neue Mission):** (a) `webui.c`+`main.c:95` → vtable (§3.5);
   (b) CMake-Source-Gating §3.2 + Kconfig-`choice` §3.1; (c) `main.c` single-site-Switch +
   `s_usb`→`s_src`. **`cdc2net-s3-wifi` danach byte-identisch verifizieren** — reines
   Entkoppeln, null Funktionsänderung. **Load-bearing Schritt**, Rest ist additiv.
3. **Transparenter `source_uart.c`-Proof:** RFNETHMs `source_uart.c` auf UART-Driver-Setup +
   `source_ops`-Gerüst eindampfen (HomeMatic-Decoder `hmu_frame`/`hmu_decoder`/`bmcond`
   **löschen**, nicht „strippen"). Byte-transparente UART-Source, Bench-Loopback durch
   `bridge→sink_tcp`, per-port-Baud erreicht den Draht. Beweist die Naht end-to-end.
4. **`eul-c6-wifi` — EnOcean:** TCM515 über die UART. **Caveat:** EnOcean ist **ESP3-gerahmt**
   (Sync/Len/CRC8H/Data/CRC8D), **nicht** byte-transparent → Framing-Policy explizit entscheiden
   (ESP3-Frames roh über TCP, FHEM-seitig dekodiert, *oder* minimaler ESP3-Framer). EULFW32s
   `Tcm515`/`Esp3Parser` als **Verhaltens-Blueprint** (Arduino → IDF neu). Dann `eul-c3-wifi`
   = gleiche Source auf 4M-Partition.
5. **`ser2thread-rcp-s3-wifi` — billigster Thread, reused Naht:** ein externer C6/H2 mit stock
   IDF `ot_rcp` erscheint als **Spinel-Serial-Device** → auf S3 „just another serial source"
   in die bestehende Bridge, **kein** RF-Code/Framework auf dem Host, byte-transparent
   (`source_usb`/`source_uart` verbatim, **kein** `source_rcp.c`, **kein** `openthread`-REQUIRE).
   Validiert das „Radio-als-Serial-Peripheral"-Modell mit null 15.4-Code im Host. Passt zur
   dokumentierten ESP32-H2-RCP-Notiz (`RFNETHM/docs/_internal/decisions.md`).
   *(Separat davon: „S3-als-OT-host" = lokaler OpenThread-IP-Stack `RADIO_SPINEL_UART` + neue
   Komponente, NICHT byte-transparent — andere Sache als der pure Tunnel.)*
6. **`ser2thread-c6-wifi` — on-die Thread zuletzt:** natives 15.4 + WiFi-Coex. Single-RF-Kosten
   akzeptieren (15.4-RX niedrigste Prio → Paketverlust unter WiFi-Last; Espressif
   produktisiert den **Dual-SoC-RCP-Split** = Schritt 5). Convenience-Single-Chip-Option,
   **nicht** die produktionsreife Thread-Topologie.
7. **W5500-ETH — linienweit (`cdc2net-s3-eth` M5 **und** `eul-c6-eth`/`tul-c6-eth`):** der
   `esp_eth`/W5500-SPI-Treiber + die `net.c`-Bearer-Abstraktion (STA‖ETH) sind **gemeinsam**;
   nur die SPI-Pin-Defs sind per-Board (S3: `RFNETHM/docs/ethernet_addition.md`; C6:
   busware-FPC-Schematic / EULFW32, CS=GPIO17). Weil die C6-Generation den FPC-W5500-Header
   trägt, dient dieses eine Feature **beiden Linien** — ETH ist nicht mehr S3-exklusiv. **`tul-*`**
   (NCN5130/KNX-Decoder) existiert in keinem Tree (aspirational).
8. **`ser2thread-c5-wifi`** bleibt **blocked** bis (a) C5-board-JSON auf der Plattform (oder
   Plattform-Bump) **und** (b) C5-natives-15.4 am Bench bestätigt. Nicht gegen `@6.13.0` planen.

## 5. Noch zu erstellen / aspirational (explizit)

- Neu: `source_uart.c` (transparent), `source_thread.c`, `partitions_c6.csv`,
  `partitions_c3_4m.csv`, `Kconfig.projbuild` + CMake-Gating — **existieren nicht**.
- `tul-*` (KNX, keine Impl), `ser2thread-c5-wifi` (kein board-id + junges C5-15.4),
  `cdc2net-s3-eth` (M5) — aspirational/blocked.
- C6/C5-Flash-Größen sind **Annahmen** — am Produktionsmodul bestätigen.
