#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="$PWD/build/scotland2/aot-hooks"
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
mkdir -p "$OUT"
"$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang" \
  -shared -nostdlib -Wl,-z,max-page-size=16384 -Wl,--hash-style=sysv \
  ports/scotland2/tests/aot-fixture.S -o "$OUT/libsc2_hook_fixture.so"
make build/klepton-ld build/t_scotland2_aot
./build/klepton-ld "$OUT/libsc2_hook_fixture.so" --platform macos -o "$OUT/unprepared.dylib" --quiet
python3 ports/scotland2/tools/hook_manifest.py --elf "$OUT/libsc2_hook_fixture.so" \
  --translation "$OUT/unprepared.dylib" --output "$OUT/manifest.json" \
  sc2_sum10 sc2_double sc2_big sc2_literal sc2_adr sc2_adrp sc2_cbz sc2_tbz sc2_bcond sc2_tail sc2_scene_fixture sc2_load_immediate
python3 - "$OUT/manifest.json" <<'PY'
from pathlib import Path
import json,sys
p=Path(sys.argv[1]);d=json.loads(p.read_text());d['originalEntryDepth']=2;p.write_text(json.dumps(d,indent=2)+'\n')
PY
python3 ports/scotland2/tools/prepare_hooks.py --elf "$OUT/libsc2_hook_fixture.so" \
  --input "$OUT/unprepared.dylib" --manifest "$OUT/manifest.json" --output "$OUT/libsc2_hook_fixture.dylib"
codesign --force --sign - "$OUT/libsc2_hook_fixture.dylib"
KL_DYLIB_DIR="$OUT" KL_GUEST_JIT=0 ./build/t_scotland2_aot "$OUT/libsc2_hook_fixture.so"
codesign --verify --strict "$OUT/libsc2_hook_fixture.dylib"
python3 ports/scotland2/tests/aot-validation.py
