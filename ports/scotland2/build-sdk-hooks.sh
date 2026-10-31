#!/bin/bash
# Actual beatsaber-hook SDK and HookTracker original-entry integration consumer.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
OUT="$PWD/build/scotland2/android-sdk-hooks"
SDK="$PWD/build/scotland2/mod-sdk"
IL2CPP="$PWD/port-deps/sources/libil2cpp/il2cpp"
mkdir -p "$OUT"
cp port-deps/sources/bshook/shared/utils/capstone-utils.hpp port-deps/sources/bshook/shared/utils/hooking.hpp "$SDK/beatsaber-hook/shared/utils/"
patch -d "$SDK/beatsaber-hook" -p1 < ports/scotland2/bshook-sdk.patch
"$CC" -std=c++20 -O1 -g -fPIC -fvisibility=hidden -shared -DANDROID \
 -DUNITY_2021 -DNEED_UNSAFE_CSHARP -DFMT_HEADER_ONLY -DSC2_AOT_HOOKS -fdeclspec \
 -include ports/scotland2/src/android-mod-sdk-prelude.hpp \
 -I"$SDK" -I"$IL2CPP/libil2cpp" -I"$IL2CPP/external/baselib/Include" \
 -I"$IL2CPP/external/baselib/Platforms/Android/Include" \
 -Iport-deps/sources/fmt/include -Iport-deps/sources/utfcpp/source \
 ports/scotland2/tests/android-sdk-hooks.cpp \
 -Lbuild/scotland2/android-aot -lsl2_abi -Lbuild/scotland2/android-customtypes/deps -lbeatsaber-hook -lpaper2_scotland2 \
 -Wl,-z,max-page-size=16384 -Wl,-soname,libsc2_sdk_hooks.so -o "$OUT/libsc2_sdk_hooks.so"
