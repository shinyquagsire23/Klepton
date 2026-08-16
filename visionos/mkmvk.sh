#!/bin/bash
# Wrap the vendored MoltenVK slices as an XCFramework for the app to embed.
#
# The Vulkan twin of mkangle.sh, and deliberately the same shape: embedded, not
# linked. Nothing in the app references a Vulkan symbol — runtime/gfx/kl_vulkan.c
# dlopens it by path at runtime, and mvk_open() already understands both the
# bare-dylib layout the host stages and the `@rpath/MoltenVK.framework/MoltenVK`
# that resolves to @executable_path/Frameworks inside a bundle, which is the ONLY
# place AMFI will accept code from.
#
# NOT per-target, for exactly ANGLE's reason (see targets.py): MoltenVK is one
# driver for every guest that asks for Vulkan, and a guest that never asks never
# dlopens it. 4.6 MB a slice, so making it conditional would buy less than the
# per-target table row it would cost.
#
# Both slices, because the platform lives in LC_BUILD_VERSION and the simulator's
# dyld refuses a visionos-stamped image. Unlike ANGLE, neither is retargeted
# ACROSS platforms — Khronos ships native xros and xros-simulator slices, and
# `tools/mvk_retarget.sh` only lowers the deployment FLOOR (26.5 -> 1.0, in the
# Mach-O load command AND in the framework Info.plist, which is a separate
# install-time bundle validation).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
OUT="Frameworks"
MVK="$ROOT/vendor-moltenvk/out"

missing=0
for D in xros xrsim; do
  [ -f "$MVK/$D/MoltenVK.framework/MoltenVK" ] || {
    missing=1; echo "!! $MVK/$D/MoltenVK.framework/MoltenVK missing"; }
done
if [ "$missing" != 0 ]; then
  # Not a hard failure. A checkout with no MoltenVK builds an app whose
  # kl_vulkan.c refuses BY NAME the moment a guest asks for Vulkan (the
  # __has_include stub), which is the right answer for the three targets that
  # never ask. Stopping here instead would make an optional dependency
  # mandatory for every target.
  echo "!! MoltenVK is not vendored — run 'make mvk'."
  echo "   Building without it: a Vulkan guest (BONELAB) will have no renderer."
  exit 0
fi

mkdir -p "$OUT"
rm -rf "$OUT/MoltenVK.xcframework"
xcodebuild -create-xcframework \
    -framework "$MVK/xros/MoltenVK.framework" \
    -framework "$MVK/xrsim/MoltenVK.framework" \
    -output "$OUT/MoltenVK.xcframework" > /dev/null

printf '  %-26s %s bytes (xros)\n' "MoltenVK.xcframework" \
    "$(stat -f%z "$MVK/xros/MoltenVK.framework/MoltenVK")"
echo "[mkmvk] done -> $OUT/"
