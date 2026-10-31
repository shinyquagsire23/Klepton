#!/bin/bash
# Host-only actual custom-types load and game SDK target discovery.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash ports/scotland2/build-customtypes.sh
BASE="$PWD/build/scotland2/macosx-flamingo/bundle"
test -f "$BASE/Frameworks/libsl2_abi.dylib"
BUNDLE="$PWD/build/scotland2/macosx-customtypes/bundle"
mkdir -p "$BUNDLE"
cp -R "$BASE/." "$BUNDLE/"
mkdir -p "$BUNDLE/Scotland2Metadata/test-only"
for NAME in libcustom-types libsc2_customtypes_probe; do
 cp "build/scotland2/android-customtypes/$NAME.so" "$BUNDLE/Scotland2Metadata/test-only/"
 ./build/klepton-ld "build/scotland2/android-customtypes/$NAME.so" --platform macos \
   -o "$BUNDLE/Frameworks/$NAME.dylib" --quiet
 codesign --force --sign - "$BUNDLE/Frameworks/$NAME.dylib"
done
if [ "${SC2_CT_REGISTER:-0}" = 1 ]; then
 export SC2_PREPARE_CUSTOMTYPES=1 KL_SCOTLAND2_CUSTOMTYPES_REGISTER=1
 export KL_SCOTLAND2_CUSTOMTYPES_UNITY="$PWD/beatsaber140/lib/arm64-v8a/libunity.so"
 export SC2_CT_REGISTRATION_PROBE="$BUNDLE/Scotland2Metadata/test-only/libsc2_customtypes_probe.so"
fi
KL_SCOTLAND2_ABI=1 KL_GUEST_JIT=0 \
 KL_SCOTLAND2_CUSTOMTYPES_PATH="$BUNDLE/Scotland2Metadata/test-only/libcustom-types.so" \
 SC2_USE_PREPARED_BUNDLE="$BUNDLE" SC2_PREPARE_GAME_HOOK=0 \
 bash ports/scotland2/test-il2cpp.sh
