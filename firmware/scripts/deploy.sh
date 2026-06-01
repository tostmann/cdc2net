#!/usr/bin/env bash
# deploy.sh — publish the built webflasher/ to the release server.
#
# The final step of a CDC2NET release (after release.sh produced the artifacts
# and the GitHub push is done): rsync webflasher/ to the install host so
# https://install.busware.de/cdc2net/ serves the new build.
#
# The destination host is NOT hardcoded (this script is public). Provide it via
# one of, in order of precedence:
#   1. arg 1:            bash firmware/scripts/deploy.sh user@host:/var/www/install/cdc2net/
#   2. env DEPLOY_DEST:  DEPLOY_DEST=user@host:/path bash firmware/scripts/deploy.sh
#   3. firmware/scripts/deploy.conf  (gitignored, one line: DEPLOY_DEST=user@host:/path)
#
# Preflights the artifact set + MD5SUMS before sending; verifies the public URL
# after (best-effort). Does NOT build — run release.sh first.
#
# Usage:
#   bash firmware/scripts/deploy.sh                 # uses env or deploy.conf
#   bash firmware/scripts/deploy.sh host:/path/      # explicit destination

set -euo pipefail

REPO_ROOT=$(git rev-parse --show-toplevel)
SCRIPT_DIR="$REPO_ROOT/firmware/scripts"
OUT="$REPO_ROOT/webflasher"
PUBLIC_URL="${PUBLIC_URL:-https://install.busware.de/cdc2net/}"

# ───── resolve DEPLOY_DEST: arg > env > deploy.conf ──────────────────────
[ -f "$SCRIPT_DIR/deploy.conf" ] && . "$SCRIPT_DIR/deploy.conf"
DEPLOY_DEST="${1:-${DEPLOY_DEST:-}}"
if [ -z "$DEPLOY_DEST" ]; then
  cat >&2 <<'EOF'
ABORT: no deploy destination set.
  Provide it as arg, env, or a gitignored conf file:
    bash firmware/scripts/deploy.sh <host>:/var/www/install/cdc2net/
    DEPLOY_DEST=<host>:/var/www/install/cdc2net/ bash firmware/scripts/deploy.sh
    echo 'DEPLOY_DEST=<host>:/var/www/install/cdc2net/' > firmware/scripts/deploy.conf
EOF
  exit 2
fi

# ───── preflight: artifacts present + MD5 self-check ─────────────────────
REQUIRED=(factory_cdc2net_esp32s3.bin firmware.bin manifest.json MD5SUMS index.html busware_logo.png)
for f in "${REQUIRED[@]}"; do
  [ -f "$OUT/$f" ] || { echo "ABORT: webflasher/$f missing — run release.sh first" >&2; exit 1; }
done
echo "[deploy] MD5SUMS self-check"
( cd "$OUT" && md5sum -c MD5SUMS ) || { echo "ABORT: MD5SUMS mismatch — rebuild before deploy" >&2; exit 1; }

VERSION=$(grep -oE '"version"[[:space:]]*:[[:space:]]*"[^"]+"' "$OUT/manifest.json" \
          | head -1 | sed -E 's/.*"([^"]+)"$/\1/')
echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
  || { echo "ABORT: manifest version '$VERSION' is not MAJOR.MINOR.BUILD" >&2; exit 2; }
echo "[deploy] publishing webflasher v$VERSION"

# ───── rsync (checksum-based: NFS mtimes are unreliable) ─────────────────
echo "[deploy] rsync webflasher/ -> $DEPLOY_DEST"
rsync -av --checksum -e "ssh -o BatchMode=yes -o ConnectTimeout=10" \
  "$OUT"/ "$DEPLOY_DEST"

# ───── post-verify (best-effort; CDN/cache may lag) ──────────────────────
echo "[deploy] verifying ${PUBLIC_URL%/}/manifest.json"
served=$(curl -fsS --max-time 15 "${PUBLIC_URL%/}/manifest.json" 2>/dev/null \
         | grep -oE '"version"[[:space:]]*:[[:space:]]*"[^"]+"' \
         | head -1 | sed -E 's/.*"([^"]+)"$/\1/' || true)
if [ "$served" = "$VERSION" ]; then
  echo "   OK — $PUBLIC_URL serves v$served"
else
  echo "   WARN — public manifest shows '${served:-unreachable}', expected '$VERSION'" >&2
  echo "          (CDN/proxy cache lag, or wrong DEPLOY_DEST host?)" >&2
fi
echo "[deploy] done — webflasher v$VERSION live at $PUBLIC_URL"
