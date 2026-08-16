#!/bin/bash
# Stage the vendored MoltenVK slices into vendor-moltenvk/out/, lowering the
# visionOS deployment floor on the way.
#
# THE TRANSFORM HERE IS NOT ANGLE'S, and the difference is worth reading before
# editing either script. angle_retarget.sh forges a PLATFORM — it builds ANGLE
# for iOS and rewrites LC_BUILD_VERSION to visionos, because ANGLE's gn has no
# xros target at all. MoltenVK ships a real, native visionOS build, so the
# platform is already VISIONOS and is left completely alone.
#
# What is wrong is the FLOOR. The release is built against the visionOS 26.5
# SDK and stamps `minos 26.5`, and dyld refuses to load an image whose minimum
# exceeds the OS running it. This machine's SDK is 26.0
# (`xcodebuild -showsdks`), so an unmodified MoltenVK fails at load with a
# version complaint that reads like a corrupt or mis-signed framework — i.e. it
# would present as "the device rejected MoltenVK", which is the same class of
# false answer angle_retarget.sh's Info.plist half exists to avoid.
#
# Lowering it is safe rather than hopeful, and the evidence is in the same
# tarball: the iOS slice built from these very sources declares `minos 15.0`,
# so nothing in MoltenVK needs a 26.x API. But that is an argument, not a
# measurement — `make mvk-check` is what actually settles it, by linking
# against the retargeted slice with the local SDK, which is exactly where a
# genuinely absent symbol would surface.
#
# Three slices, because the development ladder has three rungs:
#   macos  the host loop, `./build/m_boot bonelab`. A plain dylib, untouched —
#          its floor is 12.0 and there is nothing to fix.
#   xrsim  the visionOS Simulator. Thinned to arm64 first: the release slice is
#          a fat x86_64+arm64 and vtool rewrites one arch at a time.
#   xros   the device, the only rung that validates AMFI.
#
# Nothing is signed here. Xcode re-signs embedded frameworks at the embed step,
# which is the same path ANGLE's retargeted frameworks take and what probe P13
# validated on device.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="vendor-moltenvk"
XCF="$ROOT/dist/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework"
OUT="$ROOT/out"

[ -d "$XCF" ] || { echo "  !! $XCF missing — run 'make mvk-fetch' first"; exit 1; }

# The floor we stamp. 1.0 rather than the local SDK version: the point is to be
# loadable on any visionOS, and a floor is not a statement about which APIs are
# used. The sdk field is derived so it tracks whatever Xcode is installed
# instead of drifting stale in a literal here.
MINOS="1.0"
SDKVER="$(xcrun --sdk xros --show-sdk-version 2>/dev/null || echo 26.0)"

rm -rf "$OUT"
mkdir -p "$OUT"

# ---- the two visionOS framework slices ----
retarget_framework() {
  local SLICE="$1" DST="$2" PLAT="$3"
  local SRC="$XCF/$SLICE/MoltenVK.framework"
  [ -f "$SRC/MoltenVK" ] || { echo "  !! $SRC/MoltenVK missing"; exit 1; }

  mkdir -p "$OUT/$DST"
  cp -R "$SRC" "$OUT/$DST/MoltenVK.framework"
  local BIN="$OUT/$DST/MoltenVK.framework/MoltenVK"

  # A fat slice has to be thinned before vtool: it rewrites a single arch, and
  # the simulator on Apple Silicon only ever loads arm64 anyway.
  if lipo -archs "$BIN" | grep -q x86_64; then
    lipo -thin arm64 -output "$BIN.arm64" "$BIN" && mv "$BIN.arm64" "$BIN"
  fi

  xcrun vtool -arch arm64 -set-build-version "$PLAT" "$MINOS" "$SDKVER" -replace \
    -output "$BIN.rt" "$BIN"
  mv "$BIN.rt" "$BIN"

  # vtool does not touch the bundle, and the plist carries the floor a SECOND
  # time. Left at 26.5 it is a bundle-validation failure at install time,
  # before dyld ever runs — the trap angle_retarget.sh records for
  # CFBundleSupportedPlatforms, one key over. Only this key is rewritten;
  # CFBundleSupportedPlatforms is already correct on a native build.
  plutil -replace MinimumOSVersion -string "$MINOS" \
    "$OUT/$DST/MoltenVK.framework/Info.plist"

  # @rpath-relative, so the app's embedded copy is what gets found. Already
  # correct upstream; asserted rather than assumed because a build-tree
  # absolute path here is a device-only failure.
  install_name_tool -id "@rpath/MoltenVK.framework/MoltenVK" "$BIN" 2>/dev/null

  printf '  %-8s ' "$DST"
  xcrun vtool -show-build "$BIN" | awk '/platform/{p=$2} /minos/{m=$2} /sdk/{s=$2}
    END{printf "%-20s minos %-6s sdk %-6s ", p, m, s}'
  printf '%s bytes\n' "$(stat -f%z "$BIN")"
}

retarget_framework xros-arm64                     xros  visionos
retarget_framework xros-arm64_x86_64-simulator    xrsim visionossim

# ---- the host slice ----
# Untouched: macOS needs no retarget, and the loose dylib is what a host run
# dlopens. Kept fat — the host is arm64, but thinning a signed-nowhere dylib
# buys nothing and lipo would only be another way to get it wrong.
mkdir -p "$OUT/macos"
cp "$ROOT/dist/MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$OUT/macos/"
printf '  %-8s %-20s minos %-6s %s bytes\n' macos MACOS \
  "$(xcrun vtool -show-build -arch arm64 "$OUT/macos/libMoltenVK.dylib" | awk '/minos/{print $2}')" \
  "$(stat -f%z "$OUT/macos/libMoltenVK.dylib")"

# ---- headers ----
# The Vulkan and MoltenVK headers, staged beside the binaries so a consumer has
# one include root rather than reaching into dist/ (which mvk-fetch rebuilds).
rm -rf "$OUT/include"
cp -R "$ROOT/dist/MoltenVK/MoltenVK/include" "$OUT/include"
printf '  %-8s %s\n' include "$(ls "$OUT/include" | tr '\n' ' ')"
