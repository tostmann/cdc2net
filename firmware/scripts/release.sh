#!/usr/bin/env bash
# release.sh — CDC2NET/EUL/TUL Release-Artefakte für den Webflasher bauen.
#
# Generalisierung des RFNETHM-Release-Skripts.  Bauweise (BOSE/RFNETHM-Style):
#   1. Akzeptiert einen RELEASE_TAG (z.B. v0.1.21) als Arg/env;
#      bei Release-Builds bekommt die Firmware den TAG als FW_VERSION_STRING
#      (version_bump.py liest RELEASE_TAG aus der env), damit factory.bin /
#      firmware.bin / manifest.json deckungsgleich sind und der
#      /api/update/check sauber vergleichen kann.
#   2. `pio run -e <ENV>` mit RELEASE_TAG-env.  ENV wählt das Build-Target
#      (Default `cdc2net` = S3-Bridge).
#   3. Generiert unter `webflasher/`:
#        - factory_<mission>_<chip>.bin  (merge_bin --flash_mode dio)
#        - firmware.bin                  (= app-only, für OTA)
#        - manifest.json                 (esp-web-tools-Schema + version-Feld)
#        - MD5SUMS
#   4. Pre-Release-Test-Assertions:
#        - sdkconfig.defaults muss DIO setzen (QIO bricht den Webflasher,
#          siehe RFNETHM memory/qio_dio_webflasher_incident.md)
#        - flasher_args.json flash_mode == dio  (ground truth des Builds)
#        - bootloader.bin Header-Byte 2 == 0x02 (DIO)
#        - factory.bin   Header-Byte 2 == 0x02 (DIO)
#
# ── OTA-Cross-Flash-Guard (warum chip/family NICHT mehr hardcoded sind) ──
#   Frühere Versionen hardcodeten esp32s3 / "ESP32-S3" überall.  Würde man
#   damit ein C3/C6-Release schneiden, käme ein Manifest heraus, das über die
#   chipFamily LÜGT — und esp-web-tools vertraut genau diesem Feld als
#   Webflash-Gate.  Ein lügendes Manifest ist der einzige Pfad, auf dem ein
#   falsches Factory-Image aufs Gerät käme (Brick).  Deshalb werden chip,
#   chipFamily, Flash-Settings und ALLE Merge-Offsets pro Build aus
#   `flasher_args.json` (+ der echten `partitions.bin` für otadata) abgeleitet
#   statt geraten → das Manifest ist immer wahrheitsgemäß, der Webflash-Guard
#   bleibt gültig.  (In-Device-OTA-Pull ist separat per-Chip-URL-gegated; IDFs
#   esp_https_ota verifiziert die chip_id ohnehin vor dem Flash-Write.)
#
# WICHTIG — Dateinamen sind nicht frei wählbar:
#   Die Firmware pullt fest verdrahtet (ota_check.h):
#     OTA_FIRMWARE_URL       = https://install.busware.de/cdc2net/firmware.bin
#     OTA_CHECK_MANIFEST_URL = https://install.busware.de/cdc2net/manifest.json
#   Deshalb MUSS die App-only-Bin exakt `firmware.bin` heißen und das
#   Manifest exakt `manifest.json`.  Das `version`-Feld im Manifest treibt
#   den /api/update/check-Vergleich und MUSS MAJOR.MINOR.BUILD sein.
#
# Was es bewusst NICHT macht:
#   - Kein automatischer git-commit, kein push, kein rsync.
#     Release-Push und Webflasher-Deploy macht der User explizit.
#
# Aufruf:
#   bash firmware/scripts/release.sh v0.1.21
#   bash firmware/scripts/release.sh v0.2.0 eul-c6
#   RELEASE_TAG=v0.1.21 ENV=cdc2net bash firmware/scripts/release.sh
#   bash firmware/scripts/release.sh          # ohne tag = dev-build (S3)

set -euo pipefail

# ───── Pfade + Target auflösen ───────────────────────────────────────────
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

ENV="${2:-${ENV:-cdc2net}}"            # 2. Positional oder env; Default S3
BUILD_DIR=${BUILD_DIR:-/root/pio-build/cdc2net-build/$ENV}
ESPTOOL=${ESPTOOL:-$(command -v esptool.py 2>/dev/null || echo "$HOME/.platformio/penv/bin/esptool.py")}
PIO=${PIO:-$HOME/.platformio/penv/bin/pio}
OUT=${OUT:-$REPO_ROOT/webflasher}

# FACTORY_NAME + FIRMWARE_NAME werden nach dem Build aus chip+mission abgeleitet.

# ───── Per-MISSION deploy dir ────────────────────────────────────────────
# Subdir = MISSION (tul/eul/…), NOT env — a mission dir holds ONE manifest with
# builds[] for every chip variant (esp-web-tools auto-selects the factory by the
# connected chip) + per-chip OTA images (firmware_<chip>.bin, so a C3 never pulls
# a C6 image).  cdc2net (S3, single-chip) stays FLAT at /cdc2net/ so the shipped
# v0.1.48 devices (flat OTA URL baked in) keep updating.  The subdir MUST match
# the per-env OTA_*_URL in platformio.ini.  deploy.sh syncs the tree.
MISSION="${ENV%%-*}"                     # cdc2net / tul / eul
if [ "$MISSION" = "cdc2net" ]; then
  OUT_TARGET="$OUT";            URL_SUBDIR=""
else
  OUT_TARGET="$OUT/$MISSION";   URL_SUBDIR="$MISSION/"
fi
case "$MISSION" in
  cdc2net) MANIFEST_NAME="CDC2NET — USB-Host CDC↔TCP Bridge (CUL)";;
  tul)     MANIFEST_NAME="TUL — KNX-TP (NCN5130) ↔ TCP Bridge";;
  eul)     MANIFEST_NAME="EUL — EnOcean (TCM515) ↔ TCP Bridge";;
  *)       MANIFEST_NAME="CDC2NET — $MISSION";;
esac

# ───── Release-Tag-Handling ──────────────────────────────────────────────
RELEASE_TAG="${1:-${RELEASE_TAG:-}}"
if [ -n "$RELEASE_TAG" ]; then
  TAG_STRIPPED="${RELEASE_TAG#v}"     # strip leading 'v' for the version field
  export RELEASE_TAG
  echo ">>> Release build (env=$ENV) with tag $RELEASE_TAG (FW_VERSION_STRING = $TAG_STRIPPED)"
else
  TAG_STRIPPED=""
  echo ">>> Dev build (env=$ENV, no RELEASE_TAG) — counter-getrieben (version.h)"
fi

# ───── Test 0 Assertions (Webflasher-Sicherheit) ────────────────────────
# DIO lebt seit dem Merge in den per-chip-Overlays (sdkconfig.defaults.<chip>);
# QIO bricht den Webflasher (RFNETHM memory/qio_dio_webflasher_incident.md).
# Pre-Build (target-agnostisch, fail-fast): KEIN QIO in irgendeiner committeten
# sdkconfig.defaults* + DIO ist irgendwo gesetzt.  Die target-GENAUE Wahrheit
# prüfen wir nach dem Build aus flasher_args.json (flash_mode==dio).
echo "[release.sh] Test 0 — kein QIO + DIO vorhanden in sdkconfig.defaults*"
SDK_FILES=( firmware/sdkconfig.defaults firmware/sdkconfig.defaults.* )
if grep -qsE '^CONFIG_ESPTOOLPY_FLASHMODE_QIO=y' "${SDK_FILES[@]}"; then
  echo "ABORT: QIO flash mode gesetzt in:" >&2
  grep -lsE '^CONFIG_ESPTOOLPY_FLASHMODE_QIO=y' "${SDK_FILES[@]}" >&2
  exit 2
fi
grep -qsE '^CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' "${SDK_FILES[@]}" || {
  echo "ABORT: kein CONFIG_ESPTOOLPY_FLASHMODE_DIO=y in sdkconfig.defaults*" >&2; exit 2; }
echo "   OK — kein QIO, DIO vorhanden"

# ───── Build ────────────────────────────────────────────────────────────
[ -x "$PIO" ] || { echo "ABORT: pio fehlt unter $PIO" >&2; exit 1; }
echo "[release.sh] pio run -e $ENV  (in $REPO_ROOT/firmware)"
(cd "$REPO_ROOT/firmware" && "$PIO" run -e "$ENV")

# Build-Artefakte da?
for f in bootloader.bin partitions.bin ota_data_initial.bin firmware.bin flasher_args.json; do
  [ -f "$BUILD_DIR/$f" ] || { echo "ABORT: $BUILD_DIR/$f fehlt nach Build" >&2; exit 1; }
done

# ───── Build-Target-Parameter aus flasher_args.json (ground truth) ──────
# chip + flash-settings + bootloader/partition/app-Offsets kommen direkt vom
# Build → kein Hardcoding, automatisch korrekt für JEDES Target (16M-S3 vs
# 4M-C3/C6 haben verschiedene Layouts).  otadata steht NICHT in flasher_args
# → aus der echten partitions.bin parsen (self-contained, kein IDF-Tool).
FA="$BUILD_DIR/flasher_args.json"
fa() { python3 -c "import json,sys;d=json.load(open('$FA'));k=sys.argv[1].split('.');v=d
for p in k: v=v[p]
print(v)" "$1"; }

CHIP=$(fa   extra_esptool_args.chip)
FMODE=$(fa  flash_settings.flash_mode)
FFREQ=$(fa  flash_settings.flash_freq)
FSIZE=$(fa  flash_settings.flash_size)
BL_OFF=$(fa  bootloader.offset)
PT_OFF=$(fa  partition-table.offset)
APP_OFF=$(fa app.offset)

# otadata-Offset (type=DATA/0x01, subtype=OTA/0x00) aus partitions.bin
OD_OFF=$(python3 - "$BUILD_DIR/partitions.bin" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
for i in range(0, len(d), 32):
    e = d[i:i+32]
    if len(e) < 32 or e[0:2] != b"\xaa\x50":   # 0x50AA LE = Eintrags-Magic; sonst Tabellenende
        break
    ptype, subtype = e[2], e[3]
    off = struct.unpack("<I", e[4:8])[0]
    if ptype == 1 and subtype == 0:            # DATA / OTA = otadata
        print(hex(off)); break
PY
)
[ -n "$OD_OFF" ] || { echo "ABORT: otadata-Partition nicht in partitions.bin gefunden" >&2; exit 2; }

# chip → esp-web-tools chipFamily
case "$CHIP" in
  esp32s3) CHIPFAMILY="ESP32-S3";;
  esp32c3) CHIPFAMILY="ESP32-C3";;
  esp32c6) CHIPFAMILY="ESP32-C6";;
  esp32s2) CHIPFAMILY="ESP32-S2";;
  esp32c2) CHIPFAMILY="ESP32-C2";;
  esp32h2) CHIPFAMILY="ESP32-H2";;
  esp32p4) CHIPFAMILY="ESP32-P4";;
  esp32c5) CHIPFAMILY="ESP32-C5";;
  esp32)   CHIPFAMILY="ESP32";;
  *) echo "ABORT: unbekannter chip '$CHIP' — chipFamily-Mapping fehlt" >&2; exit 2;;
esac

FACTORY_NAME="factory_${MISSION}_${CHIP}.bin"
# OTA app image: per-chip inside a mission dir (C3/C6 must not collide); flat
# cdc2net keeps the fixed legacy name firmware.bin (shipped v0.1.48 pulls it).
if [ "$MISSION" = "cdc2net" ]; then FIRMWARE_NAME="firmware.bin"; else FIRMWARE_NAME="firmware_${CHIP}.bin"; fi

echo "[release.sh] Target: env=$ENV chip=$CHIP family=$CHIPFAMILY flash=$FSIZE/$FMODE/$FFREQ"
echo "             offsets: bl=$BL_OFF pt=$PT_OFF otadata=$OD_OFF app=$APP_OFF"

# flash_mode == dio (ground truth des Builds, nicht nur sdkconfig)
[ "$FMODE" = "dio" ] || {
  echo "ABORT: flasher_args flash_mode=$FMODE, erwartet dio (bricht Webflasher)" >&2
  exit 2
}

# Bootloader-Header byte 2 == 0x02 (DIO)?
bl_byte2=$(xxd -p -s 2 -l 1 "$BUILD_DIR/bootloader.bin")
[ "$bl_byte2" = "02" ] || {
  echo "ABORT: bootloader.bin Header-Byte 2 = 0x$bl_byte2, erwartet 0x02 (DIO)" >&2
  exit 2
}
echo "   bootloader.bin Header-Byte 2 = 0x02  ✔"

# ───── Version resolven (TAG > version.h) ───────────────────────────────
if [ -n "$TAG_STRIPPED" ]; then
  VERSION="$TAG_STRIPPED"
else
  VERSION=$(awk '/^#define FW_VERSION_STRING / {gsub(/"/,"",$3); print $3}' firmware/src/version.h)
fi
# Manifest-version-Feld MUSS MAJOR.MINOR.BUILD sein (cmp_ver: sscanf %d.%d.%d).
echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' || {
  echo "ABORT: Version '$VERSION' ist nicht MAJOR.MINOR.BUILD — /api/update/check würde nicht vergleichen" >&2
  exit 2
}
echo "[release.sh] Release-Version: v$VERSION"

# ───── Out-Dir vorbereiten ──────────────────────────────────────────────
mkdir -p "$OUT_TARGET"
# Only THIS chip's outputs — manifest.json/MD5SUMS are merged/regenerated so the
# sibling chip (already built into this mission dir) survives.
rm -f "$OUT_TARGET/$FACTORY_NAME" "$OUT_TARGET/$FIRMWARE_NAME"

FACTORY="$OUT_TARGET/$FACTORY_NAME"
FIRMWARE="$OUT_TARGET/$FIRMWARE_NAME"

# ───── factory.bin via merge_bin --flash_mode dio ───────────────────────
# Offsets + chip + flash-size kommen aus flasher_args.json/partitions.bin →
# automatisch korrekt pro Target (S3-16M-Layout != C3/C6-4M-Layout).
echo "[release.sh] esptool merge_bin → $FACTORY_NAME"
"$ESPTOOL" --chip "$CHIP" merge_bin -o "$FACTORY" \
    --flash_mode "$FMODE" --flash_freq "$FFREQ" --flash_size "$FSIZE" \
    "$BL_OFF"   "$BUILD_DIR/bootloader.bin" \
    "$PT_OFF"   "$BUILD_DIR/partitions.bin" \
    "$OD_OFF"   "$BUILD_DIR/ota_data_initial.bin" \
    "$APP_OFF"  "$BUILD_DIR/firmware.bin" \
    > /dev/null

# factory.bin Header byte 2 muss auch 0x02 sein
fa_byte2=$(xxd -p -s 2 -l 1 "$FACTORY")
[ "$fa_byte2" = "02" ] || {
  echo "ABORT: factory.bin Header-Byte 2 = 0x$fa_byte2, erwartet 0x02 (DIO)" >&2
  exit 2
}
echo "   factory.bin Header-Byte 2 = 0x02  ✔"

# factory.bin chip_id im Image-Header muss zum Target passen (Anti-Cross-Flash:
# verhindert dass ein versehentlich falsch gemergtes Image als „dieser chip"
# ausgeliefert wird).  App-Image-Header sitzt @ APP_OFF; Byte 12 = chip_id LSB.
declare -A CHIPID=( [esp32]=0 [esp32s2]=2 [esp32c3]=5 [esp32s3]=9 [esp32c2]=12 [esp32c6]=13 [esp32h2]=16 [esp32c5]=23 [esp32p4]=18 )
want_id=${CHIPID[$CHIP]:-}
# Der chipFamily-case oben hat $CHIP bereits akzeptiert → CHIPID MUSS ihn auch
# kennen, sonst fiele der Cross-Flash-Assert für genau dieses Target STILL aus
# (Map-Drift).  Darum hart abbrechen statt überspringen.
[ -n "$want_id" ] || {
  echo "ABORT: chip_id für '$CHIP' fehlt in CHIPID-Map — Cross-Flash-Assert würde still ausfallen" >&2
  exit 2
}
got_id=$(printf '%d' "0x$(xxd -p -s 12 -l 1 "$BUILD_DIR/firmware.bin")")
[ "$got_id" = "$want_id" ] || {
  echo "ABORT: firmware.bin chip_id=$got_id, erwartet $want_id für $CHIP (Cross-Flash-Schutz)" >&2
  exit 2
}
echo "   firmware.bin chip_id = $got_id ($CHIP)  ✔"

# OTA-URL ↔ Deploy-Ort Invariante: die ins Image gebackene OTA_FIRMWARE_URL MUSS
# genau dorthin zeigen, wohin release.sh/deploy.sh dieses Target legen — sonst
# pullt das Gerät im Feld die falsche (Cross-Mission) oder gar keine Firmware.
# Fängt auch ein kaputtes/fehlendes per-env -D OTA_FIRMWARE_URL sofort ab (dann
# stünde die flache Default-URL im Image statt der Subdir-URL).
# Prüfe BEIDE per-build OTA-URLs: die Firmware-Pull-URL UND die Manifest-URL
# (letztere treibt /api/update/check).  Beide defaulten flach (ota_check.h) und
# werden per non-flat env via -D auf den Subdir gezogen — ein vergessenes/
# vertipptes -D bei EINER der beiden würde sonst still den falschen Kanal
# ausliefern.  strings EINMAL in eine Var (Pipe in `grep -q` würde strings
# SIGPIPE'n → unter `set -o pipefail` Fehlalarm trotz Match).
FW_STRINGS=$(strings "$BUILD_DIR/firmware.bin")
assert_url() {   # $1 = macro name, $2 = leaf (firmware.bin | manifest.json)
  local want="https://install.busware.de/cdc2net/${URL_SUBDIR}$2"
  if ! grep -qF "$want" <<<"$FW_STRINGS"; then
    echo "ABORT: firmware.bin enthält nicht die erwartete $1:" >&2
    echo "       '$want'  (per-env -D $1 fehlt/weicht ab?)" >&2
    echo "       gefunden: $(grep -oE "https://install.busware.de/cdc2net/[^\"]*$2" <<<"$FW_STRINGS" | head -1)" >&2
    exit 2
  fi
  echo "   $1 im Image = $want  ✔"
}
assert_url OTA_FIRMWARE_URL       "$FIRMWARE_NAME"
assert_url OTA_CHECK_MANIFEST_URL manifest.json

# firmware.bin nur kopieren (= app-only für OTA; Name = OTA_FIRMWARE_URL)
cp "$BUILD_DIR/firmware.bin" "$FIRMWARE"

# ───── manifest.json — MERGE this chip into the mission manifest ─────────
# One manifest per mission: builds[] (esp-web-tools picks the factory by the
# connected chip) + ota.<chipFamily>.path (this chip's app image).  The helper
# keeps the sibling chip's entry and hard-ABORTS on a version clash (a mission
# ships under ONE version).  The top-level `version` drives /api/update/check.
FW_MD5=$(md5sum "$FIRMWARE" | awk '{print $1}')
python3 "$REPO_ROOT/firmware/scripts/merge_manifest.py" \
    "$OUT_TARGET/manifest.json" "$MANIFEST_NAME" "$VERSION" \
    "$CHIPFAMILY" "$FACTORY_NAME" "$FIRMWARE_NAME" "$FW_MD5"

# ───── MD5SUMS — regenerate from ALL files in the (merged) target dir ────
( cd "$OUT_TARGET" && md5sum -- *.bin manifest.json > MD5SUMS )

# ───── Summary ──────────────────────────────────────────────────────────
echo
echo "=== Release artefacts (env $ENV, $CHIPFAMILY, version $VERSION) ==="
ls -lh "$OUT_TARGET/$FACTORY_NAME" "$OUT_TARGET/$FIRMWARE_NAME" "$OUT_TARGET/manifest.json" "$OUT_TARGET/MD5SUMS"
echo
echo "Published under:  webflasher/${URL_SUBDIR}  ->  https://install.busware.de/cdc2net/${URL_SUBDIR}"
echo "Deploy to the release server (final release step):"
echo "  bash firmware/scripts/deploy.sh    # DEPLOY_DEST via env/arg or firmware/scripts/deploy.conf"
echo
if [ -n "$RELEASE_TAG" ]; then
echo "Nach erfolgreichem Webflasher-Deploy:"
echo "  git tag -a $RELEASE_TAG -m \"release $RELEASE_TAG\""
echo "  git push origin $RELEASE_TAG"
fi
