#!/bin/bash
# Fetch the pinned MoltenVK prebuilt release — the host side of a synthetic
# `libvulkan.so` (notes/BONELAB.md: BONELAB boots completely and cannot render
# because its graphics API is Vulkan).
#
# PREBUILT, not a source vendor, and that is a deliberate asymmetry with ANGLE.
# ANGLE lives in vendor/ as a checkout because we PATCH it — the rate-map
# registry in angle-patches/klepton.patch is ours, so the sources have to be
# there. Nothing patches MoltenVK, so a 180 MB tarball buys exactly what an
# ~18 GiB checkout and a 30-60 minute build would.
#
# `MoltenVK-all.tar`, NOT `MoltenVK-ios.tar`, and this is the finding that makes
# the whole thing cheap: the release ships **native `xros-arm64` and
# `xros-arm64_x86_64-simulator` slices**, already stamped VISIONOS /
# VISIONOSSIMULATOR in LC_BUILD_VERSION. None of angle_retarget.sh's
# iOS->visionOS platform forgery is needed or wanted here. `MoltenVK-ios.tar`
# carries an ios-arm64 device slice and nothing else — reaching visionOS from it
# would mean faking a platform that Khronos already builds for real.
#
# It still needs tools/mvk_retarget.sh afterwards, for an unrelated reason that
# script's header explains: the version FLOOR, not the platform.
#
# Nothing downloaded here is committed. vendor-moltenvk/ is gitignored exactly
# as vendor/ is; the tracked artifacts are this script and the pin below. That
# is also why the sha256 is here — a prebuilt binary dependency with no pinned
# hash is a build that silently changes under you when upstream re-cuts a tag.
set -euo pipefail
cd "$(dirname "$0")/.."

MVK_VERSION="v1.4.2"
MVK_SHA256="562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276"
MVK_URL="https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK_VERSION/MoltenVK-all.tar"

ROOT="vendor-moltenvk"
TAR="$ROOT/MoltenVK-all-$MVK_VERSION.tar"
DIST="$ROOT/dist"
STAMP="$ROOT/.klepton-fetched-$MVK_VERSION"

if [ -f "$STAMP" ]; then
  echo "  [mvk] $MVK_VERSION already fetched — $DIST"
  exit 0
fi

mkdir -p "$ROOT"

# The tarball is kept after extraction so a re-extract costs nothing, the same
# way the ANGLE clone happens once. Re-verify the hash even on a cached file:
# a truncated download from an interrupted run is otherwise indistinguishable
# from a good one, and tar would fail much later with something unhelpful.
if [ ! -f "$TAR" ]; then
  echo "  [mvk] downloading MoltenVK $MVK_VERSION (~180 MB)"
  curl -fL --progress-bar -o "$TAR.part" "$MVK_URL"
  mv "$TAR.part" "$TAR"
fi

echo "  [mvk] verifying sha256"
GOT="$(shasum -a 256 "$TAR" | awk '{print $1}')"
if [ "$GOT" != "$MVK_SHA256" ]; then
  echo "  !! sha256 mismatch for $TAR"
  echo "     expected $MVK_SHA256"
  echo "     got      $GOT"
  echo "     Delete it and re-run, or update the pin in $0 deliberately."
  exit 1
fi

echo "  [mvk] extracting -> $DIST"
rm -rf "$DIST"
mkdir -p "$DIST"
tar xf "$TAR" -C "$DIST"

# Fail here rather than in the retarget: if upstream ever drops the native
# visionOS slices, that is a decision about this whole approach and it should
# say so by name instead of surfacing as a missing directory two steps later.
for s in xros-arm64 xros-arm64_x86_64-simulator macos-arm64_x86_64; do
  [ -d "$DIST/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework/$s" ] || {
    echo "  !! $MVK_VERSION has no '$s' slice — this release cannot serve visionOS natively"
    exit 1; }
done

date > "$STAMP"
printf '  [mvk] %s ready — slices: ' "$MVK_VERSION"
ls "$DIST/MoltenVK/MoltenVK/dynamic/MoltenVK.xcframework" | grep -v Info.plist | tr '\n' ' '
echo
