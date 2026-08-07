#!/bin/bash
# Retarget an iOS ANGLE build to a visionOS platform (PLANNING §12.1(1)).
#
# Two things have to change, and only one of them is the famous one:
#
#   1. the Mach-O's LC_BUILD_VERSION — `vtool -set-build-version`. This is the
#      load-time gate dyld gets to, and the trick ALVR's repack script uses.
#   2. the framework's Info.plist. vtool does not touch the bundle, so ANGLE's
#      plist still says CFBundleSupportedPlatforms=iPhoneOS. That is a
#      *bundle-validation* failure at install time, before dyld ever runs — and
#      it would present as "the device rejected ANGLE", which is exactly the
#      answer P5.2 is trying to measure. So it gets rewritten too.
#
# Usage: angle_retarget.sh <src-outdir> <dst-outdir> <vtool-platform> <plist-platform>
#   e.g. angle_retarget.sh ios    xros  visionos    XROS
#        angle_retarget.sh ios-sim xrsim visionossim XRSimulator
set -euo pipefail
cd "$(dirname "$0")/.."

SRC="vendor/out/$1"
DST="vendor/out/$2"
VTOOL_PLAT="$3"
PLIST_PLAT="$4"

for n in libEGL libGLESv2; do
  [ -f "$SRC/$n.framework/$n" ] || {
    echo "  !! $SRC/$n.framework/$n missing — build it first"; exit 1; }
  rm -rf "$DST/$n.framework"
  mkdir -p "$DST/$n.framework"
  xcrun vtool -arch arm64 -set-build-version "$VTOOL_PLAT" 1.0 26.0 -replace \
    -output "$DST/$n.framework/$n" "$SRC/$n.framework/$n"
  # install_name must be @rpath-relative to the framework, so the app's
  # embedded copy is what gets found rather than a build-tree absolute path.
  install_name_tool -id "@rpath/$n.framework/$n" "$DST/$n.framework/$n"
  cat > "$DST/$n.framework/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>$n</string>
  <key>CFBundleIdentifier</key><string>org.chromium.ost.$n</string>
  <key>CFBundleName</key><string>$n</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$PLIST_PLAT</string></array>
  <key>MinimumOSVersion</key><string>1.0</string>
</dict></plist>
EOF
  printf '  %-12s ' "$n"
  xcrun vtool -show-build "$DST/$n.framework/$n" | awk '/platform/{printf "%s  ", $2}'
  printf 'install_name=%s\n' \
    "$(otool -D "$DST/$n.framework/$n" | tail -1)"
done
