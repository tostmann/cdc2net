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
# Capture an env-provided DEPLOY_DEST BEFORE sourcing the conf — otherwise the
# conf's own DEPLOY_DEST= would clobber it and env could never win (which would
# invert the documented arg > env > conf precedence, risking a wrong-host push).
_ENV_DEST="${DEPLOY_DEST:-}"
[ -f "$SCRIPT_DIR/deploy.conf" ] && . "$SCRIPT_DIR/deploy.conf"
DEPLOY_DEST="${1:-${_ENV_DEST:-${DEPLOY_DEST:-}}}"
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

# ───── preflight: landing page + discover per-target bundles ─────────────
# The flat landing page is always required.  Each build target is a self-
# contained bundle (manifest.json + MD5SUMS + factory + firmware.bin): the
# cdc2net(S3) set at webflasher/ root, every other env at webflasher/<env>/.
# We publish whatever release.sh has produced — one target or several.
for f in index.html busware_logo.png; do
  [ -f "$OUT/$f" ] || { echo "ABORT: webflasher/$f missing" >&2; exit 1; }
done

mapfile -t MANIFESTS < <(find "$OUT" -name manifest.json | sort)
[ "${#MANIFESTS[@]}" -gt 0 ] \
  || { echo "ABORT: no manifest.json under webflasher/ — run release.sh first" >&2; exit 1; }

declare -a TARGET_REL TARGET_VER
echo "[deploy] targets to publish:"
for mf in "${MANIFESTS[@]}"; do
  d=$(dirname "$mf")
  rel=${d#"$OUT"}; rel=${rel#/}                 # "" for flat (S3), "<env>" for subdir
  [ -f "$d/MD5SUMS" ] || { echo "ABORT: ${rel:-<root>}/MD5SUMS missing next to manifest.json" >&2; exit 1; }
  ( cd "$d" && md5sum -c MD5SUMS >/dev/null ) \
    || { echo "ABORT: MD5SUMS mismatch in ${rel:-<root>} — rebuild before deploy" >&2; exit 1; }
  v=$(grep -oE '"version"[[:space:]]*:[[:space:]]*"[^"]+"' "$mf" | head -1 | sed -E 's/.*"([^"]+)"$/\1/')
  echo "$v" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$' \
    || { echo "ABORT: ${rel:-<root>} manifest version '$v' is not MAJOR.MINOR.BUILD" >&2; exit 2; }
  TARGET_REL+=("$rel"); TARGET_VER+=("$v")
  echo "   ${rel:-<root>}  v$v  ✔ (md5 ok)"
done

# ───── rsync (checksum-based: NFS mtimes are unreliable) ─────────────────
echo "[deploy] rsync webflasher/ -> $DEPLOY_DEST"
rsync -av --checksum -e "ssh -o BatchMode=yes -o ConnectTimeout=10" \
  "$OUT"/ "$DEPLOY_DEST"

# ───── post-verify per target (best-effort; CDN/cache may lag) ───────────
for i in "${!TARGET_REL[@]}"; do
  rel="${TARGET_REL[$i]}"; want="${TARGET_VER[$i]}"
  url="${PUBLIC_URL%/}/${rel:+$rel/}manifest.json"
  served=$(curl -fsS --max-time 15 "$url" 2>/dev/null \
           | grep -oE '"version"[[:space:]]*:[[:space:]]*"[^"]+"' \
           | head -1 | sed -E 's/.*"([^"]+)"$/\1/' || true)
  if [ "$served" = "$want" ]; then
    echo "[deploy] OK — $url serves v$served"
  else
    echo "[deploy] WARN — $url shows '${served:-unreachable}', expected '$want'" >&2
    echo "               (CDN/proxy cache lag, or wrong DEPLOY_DEST host?)" >&2
  fi
done
echo "[deploy] done — ${#TARGET_REL[@]} target(s) published to $PUBLIC_URL"
