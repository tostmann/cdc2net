#!/usr/bin/env bash
# release.sh — CDC2NET Release-Artefakte für den Webflasher bauen.
#
# Generalisierung des RFNETHM-Release-Skripts.  Bauweise (BOSE/RFNETHM-Style):
#   1. Akzeptiert einen RELEASE_TAG (z.B. v0.1.21) als Arg oder env;
#      bei Release-Builds bekommt die Firmware den TAG als FW_VERSION_STRING
#      (version_bump.py liest RELEASE_TAG aus der env), damit factory.bin /
#      firmware.bin / manifest.json deckungsgleich sind und der
#      /api/update/check sauber vergleichen kann.
#   2. `pio run -e cdc2net` mit RELEASE_TAG-env.
#   3. Generiert unter `webflasher/`:
#        - factory_cdc2net_esp32s3.bin   (merge_bin --flash_mode dio)
#        - firmware.bin                   (= app-only, für OTA)
#        - manifest.json                  (esp-web-tools-Schema + version-Feld)
#        - MD5SUMS
#   4. Pre-Release-Test-Assertions:
#        - sdkconfig.defaults muss DIO setzen (QIO bricht den Webflasher,
#          siehe RFNETHM memory/qio_dio_webflasher_incident.md)
#        - bootloader.bin Header-Byte 2 == 0x02 (DIO)
#        - factory.bin   Header-Byte 2 == 0x02 (DIO)
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
#   RELEASE_TAG=v0.1.21 bash firmware/scripts/release.sh
#   bash firmware/scripts/release.sh          # ohne tag = dev-build
#                                             # (counter-getrieben, version.h)

set -euo pipefail

# ───── Pfade auflösen ────────────────────────────────────────────────────
REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

BUILD_DIR=${BUILD_DIR:-/root/pio-build/cdc2net-build/cdc2net}
ESPTOOL=${ESPTOOL:-$(command -v esptool.py 2>/dev/null || echo "$HOME/.platformio/penv/bin/esptool.py")}
PIO=${PIO:-$HOME/.platformio/penv/bin/pio}
OUT=$REPO_ROOT/webflasher
SDKCONFIG_D=firmware/sdkconfig.defaults

FACTORY_NAME=factory_cdc2net_esp32s3.bin
# firmware.bin name is FIXED by OTA_FIRMWARE_URL — do not rename.
FIRMWARE_NAME=firmware.bin

# ───── Release-Tag-Handling ──────────────────────────────────────────────
RELEASE_TAG="${1:-${RELEASE_TAG:-}}"
if [ -n "$RELEASE_TAG" ]; then
  TAG_STRIPPED="${RELEASE_TAG#v}"     # strip leading 'v' for the version field
  export RELEASE_TAG
  echo ">>> Release build with tag $RELEASE_TAG (FW_VERSION_STRING = $TAG_STRIPPED)"
else
  TAG_STRIPPED=""
  echo ">>> Dev build (no RELEASE_TAG) — counter-getrieben (version.h)"
fi

# ───── Test 0 Assertions (Webflasher-Sicherheit) ────────────────────────
# sdkconfig.defaults ist die einzige committete sdkconfig (sdkconfig.cdc2net
# wird daraus generiert und ist gitignored) → der DIO-Check greift hier an
# der Quelle.
assert_dio_config() {
  local file=$1
  if grep -qE '^CONFIG_ESPTOOLPY_FLASHMODE_QIO=y' "$file"; then
    echo "ABORT: $file enthält CONFIG_ESPTOOLPY_FLASHMODE_QIO=y (bricht Webflasher)" >&2
    exit 2
  fi
  if ! grep -qE '^CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' "$file"; then
    echo "ABORT: $file enthält kein CONFIG_ESPTOOLPY_FLASHMODE_DIO=y" >&2
    exit 2
  fi
}
echo "[release.sh] Test 0 — sdkconfig DIO check"
assert_dio_config "$SDKCONFIG_D"
echo "   OK — sdkconfig.defaults auf DIO"

# ───── Build ────────────────────────────────────────────────────────────
[ -x "$PIO" ] || { echo "ABORT: pio fehlt unter $PIO" >&2; exit 1; }
echo "[release.sh] pio run -e cdc2net  (in $REPO_ROOT/firmware)"
(cd "$REPO_ROOT/firmware" && "$PIO" run -e cdc2net)

# Build-Artefakte da?
for f in bootloader.bin partitions.bin ota_data_initial.bin firmware.bin; do
  [ -f "$BUILD_DIR/$f" ] || { echo "ABORT: $BUILD_DIR/$f fehlt nach Build" >&2; exit 1; }
done

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
mkdir -p "$OUT"
rm -f "$OUT/$FACTORY_NAME" "$OUT/$FIRMWARE_NAME" "$OUT/manifest.json" "$OUT/MD5SUMS"

FACTORY="$OUT/$FACTORY_NAME"
FIRMWARE="$OUT/$FIRMWARE_NAME"

# ───── factory.bin via merge_bin --flash_mode dio ───────────────────────
# Offsets aus partitions_ota.csv: bootloader 0x0, partitions 0x8000,
# ota_data 0xd000, erste App ota_0 @ 0x10000.
echo "[release.sh] esptool merge_bin → $FACTORY_NAME"
"$ESPTOOL" --chip esp32s3 merge_bin -o "$FACTORY" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0     "$BUILD_DIR/bootloader.bin" \
    0x8000  "$BUILD_DIR/partitions.bin" \
    0xd000  "$BUILD_DIR/ota_data_initial.bin" \
    0x10000 "$BUILD_DIR/firmware.bin" \
    > /dev/null

# factory.bin Header byte 2 muss auch 0x02 sein
fa_byte2=$(xxd -p -s 2 -l 1 "$FACTORY")
[ "$fa_byte2" = "02" ] || {
  echo "ABORT: factory.bin Header-Byte 2 = 0x$fa_byte2, erwartet 0x02 (DIO)" >&2
  exit 2
}
echo "   factory.bin Header-Byte 2 = 0x02  ✔"

# firmware.bin nur kopieren (= app-only für OTA; Name = OTA_FIRMWARE_URL)
cp "$BUILD_DIR/firmware.bin" "$FIRMWARE"

# ───── manifest.json (esp-web-tools v10) ────────────────────────────────
# builds[].parts[] → factory bin @ offset 0 (Web-Serial-Flash).
# ota.ESP32-S3.path = firmware.bin (informativ; die In-Device-OTA pullt
# fest verdrahtet OTA_FIRMWARE_URL).  Das top-level `version`-Feld treibt
# /api/update/check.
FW_MD5=$(md5sum "$FIRMWARE" | awk '{print $1}')
cat > "$OUT/manifest.json" <<EOF
{
  "name": "CDC2NET — USB-CDC Bridge (CUL/TUL/EUL)",
  "version": "$VERSION",
  "funding_url": "https://busware.de",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "improv": true,
      "parts": [
        {
          "path": "$FACTORY_NAME",
          "offset": 0
        }
      ]
    }
  ],
  "ota": {
    "ESP32-S3": {
      "path": "$FIRMWARE_NAME",
      "md5": "$FW_MD5"
    }
  }
}
EOF

# ───── MD5SUMS ──────────────────────────────────────────────────────────
( cd "$OUT" && md5sum "$FACTORY_NAME" "$FIRMWARE_NAME" manifest.json > MD5SUMS )

# ───── Summary ──────────────────────────────────────────────────────────
echo
echo "=== Release artefacts (version $VERSION) ==="
ls -lh "$OUT/$FACTORY_NAME" "$OUT/$FIRMWARE_NAME" "$OUT/manifest.json" "$OUT/MD5SUMS"
echo
echo "Public landing page:  https://install.busware.de/cdc2net/"
echo "Deploy to the release server (final release step):"
echo "  bash firmware/scripts/deploy.sh    # DEPLOY_DEST via env/arg or firmware/scripts/deploy.conf"
echo
if [ -n "$RELEASE_TAG" ]; then
echo "Nach erfolgreichem Webflasher-Deploy:"
echo "  git tag -a $RELEASE_TAG -m \"release $RELEASE_TAG\""
echo "  git push origin $RELEASE_TAG"
fi
