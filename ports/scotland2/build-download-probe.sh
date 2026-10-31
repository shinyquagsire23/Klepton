#!/bin/bash
# Independent consumer; all HTTP work stays inside actual released WebUtils.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
OUT="$PWD/build/scotland2/android-download-probe"
mkdir -p "$OUT"
"$CC" -std=c++20 -O1 -gline-tables-only -fPIC -fvisibility=hidden -shared -fdeclspec \
 -include ports/scotland2/src/android-mod-sdk-prelude.hpp -Iports/scotland2/beatsaverplusplus/upstream/include \
 -DWEBUTILS_NO_JSON -DWEBUTILS_NO_BSML -Iports/scotland2/web-utils/upstream/shared \
 ports/scotland2/tests/android-download-support.cpp ports/scotland2/tests/android-beatsaverplusplus-network.cpp \
 -Lport-results/coremods/elf/web-utils/0.6.8 -lweb-utils \
 -Lport-results/coremods/elf/beatsaverplusplus/0.2.1 -lbeatsaverplusplus \
 -Wl,-z,max-page-size=16384 -Wl,-soname,libsc2_download_probe.so -o "$OUT/libsc2_download_probe.so"
