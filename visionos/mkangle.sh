#!/bin/bash
# Wrap the two ANGLE frameworks as XCFrameworks for the app to embed.
#
# ANGLE is the shipping renderer, not a diagnostic (PLANNING §12.1(2)): its
# Metal backend *is* Metal and it already translates this title's GLES
# correctly. Until now it only existed in spikes/device-probe, so a device run
# fell back to the null GL driver and drew nothing.
#
# Embedded, not linked — same as the guest translations. Nothing in the app
# references an ANGLE symbol; kl_glfb.c dlopens both by path at runtime, and
# angle_dlopen() already understands the .framework layout as well as a bare
# .dylib. libEGL loads libGLESv2 itself through @rpath, which resolves to
# @executable_path/Frameworks — i.e. only from inside the bundle.
#
# Both slices exist for the same reason the guest's do: the platform lives in
# LC_BUILD_VERSION, and the simulator's dyld refuses a visionos-stamped image.
# Neither is a native build — ANGLE's gn has no xros target, so both are built
# for iOS and `vtool`-retargeted (`make angle-xros angle-xrsim`). AMFI accepts
# the result on device; that was probe P13.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
OUT="Frameworks"
LIBS="libEGL libGLESv2"

missing=0
for NAME in $LIBS; do
  for D in xros xrsim; do
    [ -f "$ROOT/vendor/out/$D/$NAME.framework/$NAME" ] || {
      missing=1; echo "!! $ROOT/vendor/out/$D/$NAME.framework/$NAME missing"; }
  done
done
[ "$missing" = 0 ] || { echo "!! run 'make angle-xros angle-xrsim' first"; exit 1; }

mkdir -p "$OUT"

for NAME in $LIBS; do
  rm -rf "$OUT/ANGLE_$NAME.xcframework"
  xcodebuild -create-xcframework \
      -framework "$ROOT/vendor/out/xros/$NAME.framework" \
      -framework "$ROOT/vendor/out/xrsim/$NAME.framework" \
      -output "$OUT/ANGLE_$NAME.xcframework" > /dev/null
  printf '  %-26s %s bytes (xros)\n' "ANGLE_$NAME.xcframework" \
      "$(stat -f%z "$ROOT/vendor/out/xros/$NAME.framework/$NAME")"
done

echo "[mkangle] done -> $OUT/"
