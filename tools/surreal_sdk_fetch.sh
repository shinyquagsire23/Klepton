#!/bin/bash
# Fetch Surreal Interactive's visionOS SDK, the pose source of a Surreal Touch
# build (visionos/Sources/KleptonSurrealSDK.swift, docs/SURREAL.md).
#
# The controllers track themselves, and their own frame drifts away from the
# headset's: with only KleptonSurrealBackend, hands ended 15-45 cm off near the
# head and 20-30 cm off in fast swings. Surreal's SDK solves that alignment
# continuously (clock sync, latency, controller-to-world and hand-to-controller
# transforms, whether the controller is held). It is a closed STATIC framework,
# Apache-2.0 (github.com/surreal-interactive/SwiftSDKBinary), ~47 MB for both
# slices — so, like MoltenVK, it is fetched and pinned rather than committed.
#
# gen_xcodeproj.py runs this by itself for a Surreal build that does not have it
# yet, so nobody configuring Surreal Touch has to know it exists. Building for
# Surreal Touch is the decision to use it: if this fails, that build stops with
# the reason (KLEPTON_SURREAL_SDK=0 builds with the old Bluetooth backend instead).
#
# The pin is a commit, and the hashes are of the files that matter — GitHub's
# generated tarballs are not byte-stable, the binaries inside them are.
set -euo pipefail
cd "$(dirname "$0")/.."

SDK_COMMIT="a42deb007c7f12afb543b235b75464e86e00125a"   # 2026-05-22, built for visionOS 26
SDK_URL="https://codeload.github.com/surreal-interactive/SwiftSDKBinary/tar.gz/$SDK_COMMIT"
NAME="surreal_interactive_openxr_framework"
SHA_PLIST="9ee646fd4acd5474efb1b5026aadbb8c54b12a5a43769db3e64792985c8bbe2e"
SHA_DEVICE="6f1770cdd38824a4fdf756325cd6fc21b0e77780aa0dd57122297ceef29b81f1"
SHA_SIM="5f234707ff468cf1345670c134654e3da3e65aaafd835b24c33de2dbd05b04e0"

OUT="visionos/Frameworks/$NAME.xcframework"
STAMP="$OUT/.klepton-fetched-$SDK_COMMIT"

if [ -f "$STAMP" ]; then
  echo "  [surreal-sdk] ${SDK_COMMIT:0:7} already fetched — $OUT"
  exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "  [surreal-sdk] downloading Surreal Interactive's SDK ${SDK_COMMIT:0:7} (~50 MB)"
curl -fL --progress-bar -o "$WORK/sdk.tar.gz" "$SDK_URL"
tar -xzf "$WORK/sdk.tar.gz" -C "$WORK"
SRC="$WORK/SwiftSDKBinary-$SDK_COMMIT/$NAME.xcframework"

check() {
  local got
  got="$(shasum -a 256 "$1" | awk '{print $1}')"
  if [ "$got" != "$2" ]; then
    echo "  !! sha256 mismatch for ${1#$WORK/}"
    echo "     expected $2"
    echo "     got      $got"
    exit 1
  fi
}
echo "  [surreal-sdk] verifying sha256"
check "$SRC/Info.plist" "$SHA_PLIST"
check "$SRC/xros-arm64/$NAME.framework/$NAME" "$SHA_DEVICE"
check "$SRC/xros-arm64-simulator/$NAME.framework/$NAME" "$SHA_SIM"

rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
cp -R "$SRC" "$OUT"
cp "$WORK/SwiftSDKBinary-$SDK_COMMIT/LICENSE" "$OUT/LICENSE"
touch "$STAMP"
echo "  [surreal-sdk] ready — $OUT"
