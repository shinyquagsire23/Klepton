#!/bin/bash
# Real Beat Saber IL2CPP + native Scotland2 core, on the Mac only.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [ -n "${SC2_USE_PREPARED_BUNDLE:-}" ]; then
  FRAMEWORKS="$SC2_USE_PREPARED_BUNDLE/Frameworks"
  test -f "$FRAMEWORKS/Scotland2Core.framework/Scotland2Core"
else
  bash ports/scotland2/package.sh macosx
  FRAMEWORKS="$PWD/build/scotland2/macosx/bundle/Frameworks"
  if [ "${SC2_TEST_SET:-tinyxml2}" = paper ]; then
    FRAMEWORKS="$PWD/build/scotland2/macosx-paper/bundle/Frameworks"
  fi
fi
make build/t_scotland2_il2cpp
./build/klepton-ld beatsaber140/lib/arm64-v8a/libil2cpp.so --platform macos \
  -o "$FRAMEWORKS/libil2cpp.dylib" --quiet
if [ "${SC2_PREPARE_CUSTOMTYPES:-0}" = 1 ]; then
  bash ports/scotland2/prepare-customtypes-host.sh "$FRAMEWORKS"
elif [ "${SC2_PREPARE_GAME_HOOK:-0}" = 1 ]; then
  mv "$FRAMEWORKS/libil2cpp.dylib" "$FRAMEWORKS/libil2cpp.unprepared.dylib"
  python3 ports/scotland2/tools/hook_manifest.py --elf beatsaber140/lib/arm64-v8a/libil2cpp.so \
    --translation "$FRAMEWORKS/libil2cpp.unprepared.dylib" --output "$FRAMEWORKS/libil2cpp-hook-manifest.json" \
    il2cpp_domain_get
  python3 ports/scotland2/tools/prepare_hooks.py --elf beatsaber140/lib/arm64-v8a/libil2cpp.so \
    --input "$FRAMEWORKS/libil2cpp.unprepared.dylib" --manifest "$FRAMEWORKS/libil2cpp-hook-manifest.json" \
    --output "$FRAMEWORKS/libil2cpp.dylib"
fi
codesign --force --sign - "$FRAMEWORKS/libil2cpp.dylib"
USERDATA=$(mktemp -d "$PWD/build/scotland2/il2cpp-userdata-XXXXXXXX")
KL_SCOTLAND2=1 KL_DYLIB_DIR="$FRAMEWORKS" KL_FILES_DIR="$USERDATA" \
  ./build/t_scotland2_il2cpp "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" "$PWD/beatsaber140/assets/bin/Data/Managed"
