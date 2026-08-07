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
# Idempotent, but not incremental: it replaces what is there. `beatsaber.apk`
# is load-bearing on its own, not just the unpacked tree — getPackageCodePath()
# hands Unity the APK path and it opens it as a zip.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-dev.klepton.app}"
APK="$ROOT/beatsaber.apk"
ASSETS="$ROOT/beatsaber/assets"

[ -d "$ASSETS" ] || { echo "!! $ASSETS missing"; exit 1; }
[ -f "$APK" ]    || { echo "!! $APK missing"; exit 1; }

TARGET="${1:-}"

if [ -z "$TARGET" ]; then
  UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
  [ -n "$UDID" ] || { echo "!! no booted simulator"; exit 1; }
  CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
  DEST="$CONTAINER/Documents"
  echo "[stage] simulator $UDID -> $DEST"
  mkdir -p "$DEST/beatsaber"
  rm -rf "$DEST/beatsaber/assets"
  # A copy, not a symlink: the guest resolves paths by concatenation (trap 6c)
  # and a link would work here but hide a real failure on device.
  cp -R "$ASSETS" "$DEST/beatsaber/assets"
  cp "$APK" "$DEST/beatsaber.apk"
  echo "[stage] done: $(du -sh "$DEST" | cut -f1)"
  exit 0
fi

echo "[stage] device $TARGET (this uploads ~2.2 GB and takes a while)"
copy() {   # <source> <destination-relative-to-container>
  xcrun devicectl device copy to --device "$TARGET" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$1" --destination "$2"
}
copy "$ASSETS" "Documents/beatsaber/assets"
copy "$APK"    "Documents/beatsaber.apk"
echo "[stage] done"
