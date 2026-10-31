#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
BUNDLE="$PWD/build/scotland2/macosx-bad-import/bundle"
BASE="$PWD/build/scotland2/macosx-customtypes/bundle"
test -f "$BASE/Frameworks/Scotland2Core.framework/Scotland2Core"
mkdir -p "$BUNDLE"
cp -R "$BASE/." "$BUNDLE/"
MODULE="$BUNDLE/Scotland2Metadata/libs/libsc2_bad_import.so"
"$CC" -shared -fPIC -O1 ports/scotland2/tests/android-bad-import.c \
  -Wl,-soname,libsc2_bad_import.so -o "$MODULE"
./build/klepton-ld "$MODULE" --platform macos -o "$BUNDLE/Frameworks/libsc2_bad_import.dylib" --quiet
codesign --force --sign - "$BUNDLE/Frameworks/libsc2_bad_import.dylib"
make build/t_scotland2_il2cpp
USERDATA=$(mktemp -d "$PWD/build/scotland2/rejected-import-XXXXXXXX")
KL_SCOTLAND2=1 KL_SCOTLAND2_ABI=1 KL_GUEST_JIT=0 SC2_TEST_REJECT_IMPORT=1 \
 KL_DYLIB_DIR="$BUNDLE/Frameworks" KL_FILES_DIR="$USERDATA" \
 ./build/t_scotland2_il2cpp "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" "$PWD/beatsaber140/assets/bin/Data/Managed"
