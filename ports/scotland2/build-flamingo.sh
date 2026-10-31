#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
REPRO_FLAGS=(-fdebug-compilation-dir=.)
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  REPRO_FLAGS=("-ffile-prefix-map=$PWD=." "-fmacro-prefix-map=$PWD=." "-ffile-prefix-map=$NDK=/ndk" -fdebug-compilation-dir=.)
fi

OUT="$PWD/build/scotland2/android-flamingo"
mkdir -p "$OUT"
FLAGS=(-std=c++20 -O1 -g -shared -fPIC -fvisibility=hidden -Wall -Wextra -Werror
  -Wno-unused-parameter -Wno-unused-function -DSC2_AOT_HOOKS -DANDROID
  -DFLAMINGO_LOG_STANDALONE -DFMT_HEADER_ONLY -DFMT_USE_CONSTEVAL=0
  -Iports/scotland2/flamingo/upstream/shared -Iport-deps/sources/fmt/include
  -Wl,-z,max-page-size=16384)
"$CC" "${REPRO_FLAGS[@]}" "${FLAGS[@]}" ports/scotland2/flamingo/upstream/src/installer.cpp \
  ports/scotland2/flamingo/upstream/src/capi.cpp ports/scotland2/src/android-flamingo-fixups.cpp \
  -llog -Wl,-soname,libsc2_flamingo.so -o "$OUT/libsc2_flamingo.so"
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  "$CC" "${REPRO_FLAGS[@]}" "${FLAGS[@]}" ports/scotland2/src/android-flamingo-scene.cpp \
    -L"$OUT" -Wl,--no-as-needed -lsc2_flamingo -Wl,-soname,libsc2_scene.so \
    -o "$OUT/libsc2_scene.so"
else
"$CC" "${REPRO_FLAGS[@]}" "${FLAGS[@]}" ports/scotland2/tests/android-flamingo.cpp ports/scotland2/src/android-flamingo-scene.cpp \
  -L"$OUT" -Wl,--no-as-needed -lsc2_flamingo -Wl,-soname,libsc2_flamingo_probe.so \
  -o "$OUT/libsc2_flamingo_probe.so"
fi
python3 - "$OUT" <<'PY'
import hashlib,json,sys
from pathlib import Path
out=Path(sys.argv[1])
record={'source':json.loads(Path('ports/scotland2/flamingo/provenance.json').read_text()),
 'outputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.so')},
 'scope':'Original Flamingo installer/C API with prepared-entry fixup backend; separate guest C++ API consumer. Midpoint hooks and runtime code generation unsupported.'}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
