#!/bin/bash
# Android ABI objects, to be translated/signed separately by the Apple packager.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64"
CC="$TOOLCHAIN/bin/aarch64-linux-android26-clang++"
REPRO_FLAGS=(-fdebug-compilation-dir=.)
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  REPRO_FLAGS=("-ffile-prefix-map=$PWD=." "-fmacro-prefix-map=$PWD=." "-ffile-prefix-map=$NDK=/ndk" -fdebug-compilation-dir=.)
fi

OUT="$PWD/build/scotland2/android-paths"
if [ "${SC2_ANDROID_CAPSTONE:-0}" = 1 ]; then OUT="$PWD/build/scotland2/android-compat"; fi
if [ "${SC2_ANDROID_AOT:-0}" = 1 ]; then OUT="$PWD/build/scotland2/android-aot"; fi
mkdir -p "$OUT"
cat > "$OUT/adapter.exports" <<'MAP'
{ global: _ZN9modloader*; cs_*; A64HookFunction; A64HookFunctionV; sc2_bshook_mkpath; sc2_beatsaver_extract; local: *; };
MAP
EXTRA=(-Wl,--version-script,"$OUT/adapter.exports")
if [ "${SC2_ANDROID_CAPSTONE:-0}" = 1 ]; then
  python3 - <<'PY'
import hashlib,json
from pathlib import Path
root=Path('port-deps/sources/capstone-qpm')
lock=json.loads((root/'lock.json').read_text())
assert hashlib.sha256((root/'libcapstone.a').read_bytes()).hexdigest()==lock['sha256']
PY
  CAPSTONE="$PWD/port-deps/sources/capstone-qpm/libcapstone.a"
  if [ "${SC2_ANDROID_AOT:-0}" = 1 ]; then
    CAPSTONE="$OUT/libcapstone-view.a"
    "$TOOLCHAIN/bin/llvm-objcopy" --redefine-sym cs_disasm_iter=sc2_capstone_iter_raw \
      --redefine-sym cs_disasm=sc2_capstone_disasm_raw \
      port-deps/sources/capstone-qpm/libcapstone.a "$CAPSTONE"
  fi
  EXTRA+=(-Wl,--whole-archive "$CAPSTONE" -Wl,--no-whole-archive)
fi
COMMON=(-std=c++20 -O1 -g -fPIC -shared -fvisibility=hidden -Wall -Wextra -Werror
  -DSL2_KLEPTON -Iports/scotland2/upstream/shared
  -Wl,-z,max-page-size=16384)
SOURCES=(ports/scotland2/src/android-paths.cpp)
if [ "${SC2_ANDROID_AOT:-0}" = 1 ]; then
  SOURCES+=(ports/scotland2/and64/shared/inline-hook/And64InlineHook.cpp)
  SOURCES+=(ports/scotland2/src/android-bshook-filesystem.cpp)
  "$TOOLCHAIN/bin/aarch64-linux-android26-clang" "${REPRO_FLAGS[@]}" -O1 -fPIC -fvisibility=hidden \
    -Iport-deps/sources/zip-songdownloader/src -c ports/scotland2/src/safe-zip.c -o "$OUT/safe-zip.o"
  SOURCES+=(ports/scotland2/src/android-beatsaver-extract.cpp "$OUT/safe-zip.o")
  COMMON+=(-DSC2_AOT_HOOKS)
  if [ "${SC2_ANDROID_CAPSTONE:-0}" = 1 ]; then
    SOURCES+=(ports/scotland2/src/android-capstone-view.cpp)
    mkdir -p "$OUT/sdk"
    ln -sfn "$PWD/port-deps/sources/capstone-qpm/source" "$OUT/sdk/capstone"
    COMMON+=(-I"$OUT/sdk")
  fi
fi
"$CC" "${REPRO_FLAGS[@]}" "${COMMON[@]}" "${SOURCES[@]}" \
  "${EXTRA[@]}" \
  -Wl,-soname,libsl2_abi.so -o "$OUT/libsl2_abi.so"
# The consumer's seven C++ calls must stay as imports to the other guest DSO.
"$CC" "${REPRO_FLAGS[@]}" "${COMMON[@]}" ports/scotland2/tests/android-path-consumer.cpp \
  -Wl,--exclude-libs,ALL \
  -L"$OUT" -Wl,--no-as-needed -lsl2_abi -Wl,-soname,libsc2_abi_consumer.so \
  -o "$OUT/libsc2_abi_consumer.so"
cp "$TOOLCHAIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/"
python3 - "$OUT" "${SC2_ANDROID_CAPSTONE:-0}" "${SC2_ANDROID_AOT:-0}" <<'PY'
import hashlib,json,sys
from pathlib import Path
sys.path.insert(0,'tools')
from audit_elf import Elf
out=Path(sys.argv[1])
adapter=Elf(str(out/'libsl2_abi.so'))
consumer=Elf(str(out/'libsc2_abi_consumer.so'))
expected={'_ZN9modloader13get_files_dirEv','_ZN9modloader16get_external_dirEv',
 '_ZN9modloader18get_application_idEv','_ZN9modloader18get_libil2cpp_pathEv',
 '_ZN9modloader18get_modloader_pathEv','_ZN9modloader25get_modloader_source_pathEv',
 '_ZN9modloader28get_modloader_root_load_pathEv'}
path_expected=set(expected)
if sys.argv[3]=='1': expected|={'A64HookFunction','A64HookFunctionV','sc2_bshook_mkpath','sc2_beatsaver_extract'}
if sys.argv[2]=='1':
 assert expected < adapter.exports and all(e.startswith('cs_') for e in adapter.exports-expected)
else:
 assert adapter.exports==expected,adapter.exports
assert path_expected <= {name for name,ver in consumer.imports}
assert adapter.p_align >= 16384 and consumer.p_align >= 16384
assert 'libc++_shared.so' in adapter.needed and 'libc++_shared.so' in consumer.needed
record={'scope':'Android C++ path/string adapters only. No native C++ aliases, registry-object bridge or hook backend.',
 'ndk':json.loads(Path('port-deps/toolchains/ndk-lock.json').read_text()),
 'outputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in out.glob('*.so')},
 'verifiedExports':sorted(adapter.exports),'consumerImportsVerified':True}
if sys.argv[2]=='1': record['capstone']=json.loads(Path('port-deps/sources/capstone-qpm/lock.json').read_text())
if sys.argv[3]=='1':
 record['bshookFilesystem']={'adapter':'ports/scotland2/src/android-bshook-filesystem.cpp','sha256':hashlib.sha256(Path('ports/scotland2/src/android-bshook-filesystem.cpp').read_bytes()).hexdigest(),'scope':'Prepared-entry replacement for upstream shell-based mkpath; guest C++ ABI and path-aware filesystem calls.'}
 record['and64']=json.loads(Path('ports/scotland2/and64/provenance.json').read_text())
 if sys.argv[2]=='1': record['capstoneOriginalView']={'adapter':'ports/scotland2/src/android-capstone-view.cpp','adapterSha256':hashlib.sha256(Path('ports/scotland2/src/android-capstone-view.cpp').read_bytes()).hexdigest(),'renamedArchiveSha256':hashlib.sha256((out/'libcapstone-view.a').read_bytes()).hexdigest(),'renamedSymbols':['cs_disasm_iter','cs_disasm'],'scope':'Original Capstone with original entry-word data/PC inspection for prepared hooks.'}
 record['scope']='Android C++ getters, original Capstone and source-ported And64 prepared-entry API. No runtime code writes; no full mod-stack compatibility claim.'
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
echo "Built Android C++ path adapters: $OUT"
