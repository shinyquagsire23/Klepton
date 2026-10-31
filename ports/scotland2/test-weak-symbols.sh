#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
OUT="$PWD/build/scotland2/weak-symbols"
BUNDLE="$PWD/build/scotland2/macosx-lapiz/bundle"
mkdir -p "$OUT"
for NAME in first second; do
  if [ "$NAME" = second ]; then DEF=-DSC2_WEAK_SECOND; else DEF=-DSC2_WEAK_FIRST; fi
  "$CC" -std=c++20 -O1 -fPIC -shared "$DEF" ports/scotland2/tests/android-weak.cpp \
    -Wl,-soname,libsc2_weak_$NAME.so -o "$OUT/libsc2_weak_$NAME.so"
  ./build/klepton-ld "$OUT/libsc2_weak_$NAME.so" --platform macos -o "$BUNDLE/Frameworks/libsc2_weak_$NAME.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libsc2_weak_$NAME.dylib"
done
make build/t_scotland2_il2cpp
USERDATA=$(mktemp -d "$PWD/build/scotland2/weak-userdata-XXXXXXXX")
KL_SCOTLAND2=1 KL_SCOTLAND2_ABI=1 KL_GUEST_JIT=0 KL_DYLIB_DIR="$BUNDLE/Frameworks" KL_FILES_DIR="$USERDATA" \
 SC2_WEAK_FIRST="$OUT/libsc2_weak_first.so" SC2_WEAK_SECOND="$OUT/libsc2_weak_second.so" \
 ./build/t_scotland2_il2cpp "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" "$PWD/beatsaber140/assets/bin/Data/Managed"
