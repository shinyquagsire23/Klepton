#!/bin/bash
# Mac-only actual Lapiz early-phase test; Unity is mapped for type hooks only.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash ports/scotland2/build-customtypes.sh
bash ports/scotland2/build-lapiz.sh
BASE="$PWD/build/scotland2/macosx-customtypes/bundle"
BUNDLE="$PWD/build/scotland2/macosx-lapiz/bundle"
test -f "$BASE/Frameworks/Scotland2Core.framework/Scotland2Core"
mkdir -p "$BUNDLE"
cp -R "$BASE/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
python3 - "$META/early_mods" <<'PY'
from pathlib import Path
import sys
assert {p.name for p in Path(sys.argv[1]).glob('*.so')} <= {'liblapiz.so'}
PY
cp build/scotland2/android-customtypes/libcustom-types.so "$META/libs/"
cp build/scotland2/android-lapiz/liblapiz.so "$META/early_mods/"
mkdir -p "$META/profiles"
cp build/scotland2/android-lapiz/hook-profile.txt "$META/profiles/lapiz-hooks.txt"
cp build/scotland2/android-customtypes/libsc2_customtypes_probe.so "$META/shims/"
for NAME in libcustom-types libsc2_customtypes_probe liblapiz; do
  if [ "$NAME" = liblapiz ]; then DIR=android-lapiz; else DIR=android-customtypes; fi
  ./build/klepton-ld "build/scotland2/$DIR/$NAME.so" --platform macos \
    -o "$BUNDLE/Frameworks/$NAME.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/$NAME.dylib"
done
KL_SCOTLAND2_ABI=1 KL_SCOTLAND2_LAPIZ_PROBE=1 KL_GUEST_JIT=0 SC2_PREPARE_CUSTOMTYPES=1 \
 SC2_LAPIZ_CATALOG="$PWD/build/scotland2/android-lapiz/hook-catalog.json" \
 KL_SCOTLAND2_CUSTOMTYPES_UNITY="$PWD/beatsaber140/lib/arm64-v8a/libunity.so" \
 SC2_USE_PREPARED_BUNDLE="$BUNDLE" SC2_PREPARE_GAME_HOOK=0 \
 bash ports/scotland2/test-il2cpp.sh
