#!/bin/bash
# Rebuild only the independent consumer; never modifies the staged SDK.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
OUT="$PWD/build/scotland2/android-customtypes"
SDK="$PWD/build/scotland2/mod-sdk"
IL2CPP="$PWD/port-deps/sources/libil2cpp/il2cpp"
"$CC" -std=c++20 -O1 -g -fPIC -fvisibility=hidden -shared -DANDROID \
 -DUNITY_2021 -DNEED_UNSAFE_CSHARP -DFMT_HEADER_ONLY -DSC2_AOT_HOOKS -fdeclspec \
 -Wno-invalid-offsetof -Wno-extra-qualification \
 -include ports/scotland2/src/android-mod-sdk-prelude.hpp \
 -Iports/scotland2/customtypes/upstream/shared -I"$SDK" -I"$IL2CPP/libil2cpp" \
 -I"$IL2CPP/external/baselib/Include" -I"$IL2CPP/external/baselib/Platforms/Android/Include" \
 -Iport-deps/sources/fmt/include -Iport-deps/sources/utfcpp/source \
 ports/scotland2/tests/android-customtypes.cpp -L"$OUT" -lcustom-types \
 -Lbuild/scotland2/android-aot -lsl2_abi -L"$OUT/deps" -lbeatsaber-hook -lpaper2_scotland2 \
 -Wl,-z,max-page-size=16384 -Wl,-soname,libsc2_customtypes_probe.so -o "$OUT/libsc2_customtypes_probe.so"
python3 - "$OUT" <<'PY'
from pathlib import Path
import hashlib,json,sys
out=Path(sys.argv[1]);p=out/'provenance.json'
if p.exists():
 d=json.loads(p.read_text());d['probeSha256']=hashlib.sha256((out/'libsc2_customtypes_probe.so').read_bytes()).hexdigest()
 p.write_text(json.dumps(d,indent=2)+'\n')
PY
