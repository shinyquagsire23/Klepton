#!/bin/bash
# A missing DT_NEEDED matters even if it supplies no imported functions.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang"
OUT="$PWD/build/scotland2/missing-dependency";mkdir -p "$OUT"
printf '%s\n' 'void sc2_unused_dependency(void) {}' > "$OUT/absent.c"
printf '%s\n' '__attribute__((constructor)) static void trap(void) { __builtin_trap(); }' > "$OUT/victim.c"
"$CC" -shared -fPIC "$OUT/absent.c" -Wl,-soname,libsc2_missing_dependency.so -o "$OUT/libsc2_missing_dependency.so"
"$CC" -shared -fPIC "$OUT/victim.c" -L"$OUT" -Wl,--no-as-needed -lsc2_missing_dependency -Wl,-soname,libsc2_missing_victim.so -o "$OUT/libsc2_missing_victim.so"
BASE="$PWD/build/scotland2/macosx-customtypes/bundle"
./build/klepton-ld "$OUT/libsc2_missing_victim.so" --platform macos -o "$BASE/Frameworks/libsc2_missing_victim.dylib" --quiet
codesign --force --sign - "$BASE/Frameworks/libsc2_missing_victim.dylib"
make build/t_scotland2_il2cpp
USERDATA=$(mktemp -d "$OUT/userdata-XXXXXXXX")
KL_SCOTLAND2=1 KL_SCOTLAND2_ABI=1 KL_GUEST_JIT=0 KL_DYLIB_DIR="$BASE/Frameworks" KL_FILES_DIR="$USERDATA" \
 SC2_TEST_MISSING_DEP="$OUT/libsc2_missing_victim.so" ./build/t_scotland2_il2cpp \
 "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" "$PWD/beatsaber140/assets/bin/Data/Managed"
