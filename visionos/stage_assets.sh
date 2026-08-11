#!/bin/bash
# Stage the guest's data into the app's Documents container.
#
# Assets go here rather than into the .app for one reason: there are 2.2 GB of
# them and they change only when the APK does, while the code changes every few
# minutes. Bundling them would put a multi-minute upload in front of every
# build, which is the difference between the M4 loop working on device and not.
# Code is the other way round — the guest libraries ride in the bundle, because
# P3 established AMFI's tolerance for a klepton-ld dylib *inside a bundle we
# signed* and established nothing about one pushed into Documents.
#
#   ./stage_assets.sh                 # the booted simulator
#   ./stage_assets.sh <device-udid>   # a physical Vision Pro
#
# Idempotent, but not incremental: it replaces what is there. The APK is
# load-bearing on its own, not just the unpacked tree — getPackageCodePath()
# hands the guest the APK path and it opens it as a zip.
#
# Which guest, and therefore which apk and which asset tree, comes from
# KLEPTON_TARGET (visionos/targets.py). The container layout mirrors the repo's:
# <container>/Documents/<tree>/assets and <container>/Documents/<tree>.apk, which
# is what kl_app_configure builds its paths from.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
APK="$ROOT/$KLT_APK"
ASSETS="$ROOT/$KLT_ASSETS"
TREE="$KLT_TREE"

[ -d "$ASSETS" ] || { echo "!! $ASSETS missing"; exit 1; }
[ -f "$APK" ]    || { echo "!! $APK missing"; exit 1; }

TARGET="${1:-}"

if [ -z "$TARGET" ]; then
  UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
  [ -n "$UDID" ] || { echo "!! no booted simulator"; exit 1; }
  CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
  DEST="$CONTAINER/Documents"
  echo "[stage] simulator $UDID -> $DEST ($KLT_NAME)"
  mkdir -p "$DEST/$TREE"
  rm -rf "$DEST/$TREE/assets"
  # A copy, not a symlink: the guest resolves paths by concatenation (trap 6c)
  # and a link would work here but hide a real failure on device.
  cp -R "$ASSETS" "$DEST/$TREE/assets"
  cp "$APK" "$DEST/$KLT_APK"
  if [ -n "$KLT_QTPLUGINS" ]; then
    rm -rf "$DEST/$TREE/qtplugins"
    mkdir -p "$DEST/$TREE/qtplugins"
    cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$DEST/$TREE/qtplugins/"
  fi
  echo "[stage] done: $(du -sh "$DEST" | cut -f1)"
  exit 0
fi

# Beat Saber's 2.2 GB is the reason any of this exists; Steam Link's is 44 MB
# and takes seconds. Saying which is which up front is the difference between
# waiting and wondering.
echo "[stage] device $TARGET, $KLT_NAME ($(du -shc "$ASSETS" "$APK" | tail -1 | cut -f1))"
copy() {   # <source> <destination-relative-to-container>
  xcrun devicectl device copy to --device "$TARGET" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$1" --destination "$2"
}
copy "$ASSETS" "Documents/$TREE/assets"
copy "$APK"    "Documents/$KLT_APK"
# The Qt plugin .so files, when the target asks for them. They are DATA, not a
# loader path — see the `qtplugins` note in targets.py: Qt globs this directory
# and parses each candidate's ELF metadata before it will dlopen it, and the
# dlopen itself still resolves to the signed framework in the bundle.
# devicectl copies a directory, so they are staged through one locally first.
if [ -n "$KLT_QTPLUGINS" ]; then
  STAGE=$(mktemp -d)/qtplugins
  mkdir -p "$STAGE"
  cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$STAGE/"
  copy "$STAGE" "Documents/$TREE/qtplugins"
  rm -rf "$(dirname "$STAGE")"
fi
echo "[stage] done"
