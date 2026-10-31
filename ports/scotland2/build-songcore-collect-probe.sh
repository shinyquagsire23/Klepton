#!/bin/bash
# Independent SongCore service/scan consumer; called only after SongCore late_load.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
IL2CPP="$PWD/port-deps/sources/libil2cpp/il2cpp"
"$CC" -std=c++20 -fno-access-control -O1 -gline-tables-only -fPIC -fvisibility=hidden -shared \
 -DANDROID -DUNITY_2021 -DHAS_CODEGEN -DNEED_UNSAFE_CSHARP -DFMT_HEADER_ONLY -DSC2_AOT_HOOKS -fdeclspec \
 -Wno-invalid-offsetof -Wno-extra-qualification \
 -include ports/scotland2/src/android-mod-sdk-prelude.hpp \
 -Iports/scotland2/songcore/upstream/shared -Ibuild/scotland2/mod-sdk -Iport-deps/sources/bs-cordl/include \
 -I"$IL2CPP/libil2cpp" -I"$IL2CPP/external/baselib/Include" -I"$IL2CPP/external/baselib/Platforms/Android/Include" \
 -Iport-deps/sources/fmt/include -Iport-deps/sources/utfcpp/source \
 ports/scotland2/tests/android-songcore-collect.cpp -Lbuild/scotland2/android-songcore -lsongcore \
 -Lbuild/scotland2/android-bsml -lbsml -Lbuild/scotland2/android-lapiz -llapiz \
 -Lbuild/scotland2/android-customtypes -lcustom-types -Lbuild/scotland2/android-customtypes/deps -lbeatsaber-hook -lpaper2_scotland2 \
 -Lbuild/scotland2/android-aot -lsl2_abi -Wl,-z,max-page-size=16384 -Wl,-soname,libsc2_songcore_collect.so \
 -o build/scotland2/android-songcore/libsc2_songcore_collect.so
