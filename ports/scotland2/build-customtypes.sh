#!/bin/bash
# Rebuild actual custom-types with the adapted And64 API; does not package/install.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
REPRO_FLAGS=(-fdebug-compilation-dir=.)
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  REPRO_FLAGS=("-ffile-prefix-map=$PWD=." "-fmacro-prefix-map=$PWD=." "-ffile-prefix-map=$NDK=/ndk" -fdebug-compilation-dir=.)
fi

DIAGNOSTIC_FLAGS=(-DSC2_DIAGNOSTICS=0)
if [ "${SC2_BUILD_MODE:-diagnostic}" != release ]; then DIAGNOSTIC_FLAGS=(-DSC2_DIAGNOSTICS=1 -DCT_USE_GCDESCRIPTOR_DEBUG); fi

OUT="$PWD/build/scotland2/android-customtypes"
SDK="$PWD/build/scotland2/mod-sdk"
SRC="$PWD/ports/scotland2/customtypes/upstream"
IL2CPP="$PWD/port-deps/sources/libil2cpp/il2cpp"
mkdir -p "$OUT" "$SDK"
python3 - "$SDK" "$OUT" <<'PY'
from pathlib import Path
import hashlib,json,shutil,sys
root=Path('port-deps/sources');sdk=Path(sys.argv[1])
out=Path(sys.argv[2]);deps=out/'deps';deps.mkdir(exist_ok=True)
lock=json.loads(Path('port-results/coremods/coremods-lock.json').read_text())
for mod in ['beatsaber-hook','paper2_scotland2']:
 binary,=[b for b in lock['binaries'] if b['mod']==mod]
 source=Path('port-results/coremods')/binary['file']
 assert hashlib.sha256(source.read_bytes()).hexdigest()==binary['sha256']
 shutil.copy2(source,deps/source.name)
for row in json.loads((root/'sdk-submodules-lock.json').read_text()):
 archive=root/(row['repository'].split('/')[-1]+'-'+row['commit'][:7]+'.tar.gz')
 assert hashlib.sha256(archive.read_bytes()).hexdigest()==row['sha256']
lock=json.loads((root/'libil2cpp-lock.json').read_text())
assert hashlib.sha256((root/'libil2cpp_0.4.0.zip').read_bytes()).hexdigest()==lock['sha256']
shutil.copytree(root/'bshook/shared',sdk/'beatsaber-hook/shared',dirs_exist_ok=True)
shutil.copytree(root/'rapidjson',sdk/'beatsaber-hook/shared/rapidjson',dirs_exist_ok=True)
for name,target in [('paper2_scotland2',root/'paperlog'),('capstone',root/'capstone-qpm/source'),('scotland2',Path('ports/scotland2/upstream'))]:
 link=sdk/name
 if not link.exists():link.symlink_to(target.resolve(),target_is_directory=True)
 assert link.resolve()==target.resolve()
PY
patch -d "$SDK/beatsaber-hook" -p1 < ports/scotland2/bshook-sdk.patch
python3 ports/scotland2/tools/find_customtypes_targets.py --il2cpp beatsaber140/lib/arm64-v8a/libil2cpp.so \
 --unity beatsaber140/lib/arm64-v8a/libunity.so --output "$OUT/targets.json"
python3 - "$OUT" <<'PY'
from pathlib import Path
import json,sys,os
out=Path(sys.argv[1]);profile=json.loads((out/'targets.json').read_text())
if os.environ.get('SC2_BUILD_MODE')=='release':
 profile['il2cpp']['targets']=[row for row in profile['il2cpp']['targets'] if not row['name'].startswith('LivenessState_')]
 profile['excludedDiagnosticHooks']='Three CT_USE_GCDESCRIPTOR_DEBUG liveness tracing hooks'
 (out/'targets.json').write_text(json.dumps(profile,indent=2)+'\n')
target,=profile['unity']['targets']
(out/'sc2-customtypes-target.h').write_text('#define SC2_CUSTOMTYPES_SCRIPTING_CLASS_OFFSET '+target['vaddr']+'\n#define SC2_CUSTOMTYPES_SCRIPTING_CLASS_WORD '+target['expect']+'u\n')
PY
"$CC" "${REPRO_FLAGS[@]}" -std=c++20 -O1 -g -fPIC -fexceptions -frtti -fvisibility=hidden -shared \
 -DVERSION='"0.18.3"' -DMOD_ID='"custom-types"' -DVERSION_LONG=1 \
 -DANDROID -DNEED_UNSAFE_CSHARP -DUNITY_2021 -DNO_VERBOSE_LOGS "${DIAGNOSTIC_FLAGS[@]}" \
 -DFMT_HEADER_ONLY -fdeclspec -Wno-invalid-offsetof -Wno-extra-qualification \
 -DSC2_AOT_HOOKS -include ports/scotland2/src/android-mod-sdk-prelude.hpp \
 -include "$OUT/sc2-customtypes-target.h" -I"$OUT" \
 -I"$SRC/shared" -I"$SRC/src" -I"$SRC/include" -I"$SDK" \
 -I"$IL2CPP/libil2cpp" -I"$IL2CPP/external/baselib/Include" \
 -I"$IL2CPP/external/baselib/Platforms/Android/Include" \
 -Iport-deps/sources/fmt/include -Iport-deps/sources/utfcpp/source \
 "$SRC/src/register.cpp" "$SRC/src/type-registration.cpp" "$SRC/src/logging.cpp" \
 "$SRC/src/coroutine.cpp" "$SRC/src/delegate.cpp" \
 ports/scotland2/src/android-customtypes-target.cpp \
 -Lbuild/scotland2/android-aot -lsl2_abi \
 -L"$OUT/deps" -lbeatsaber-hook -lpaper2_scotland2 \
 -llog -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL \
 -Wl,-soname,libcustom-types.so -o "$OUT/libcustom-types.so"
if [ "${SC2_BUILD_MODE:-diagnostic}" != release ]; then
"$CC" "${REPRO_FLAGS[@]}" -std=c++20 -O1 -g -fPIC -fvisibility=hidden -shared -DANDROID \
 -DUNITY_2021 -DNEED_UNSAFE_CSHARP -DFMT_HEADER_ONLY -DSC2_AOT_HOOKS -fdeclspec \
 -Wno-invalid-offsetof -Wno-extra-qualification \
 -include ports/scotland2/src/android-mod-sdk-prelude.hpp \
 -I"$SRC/shared" -I"$SDK" -I"$IL2CPP/libil2cpp" \
 -I"$IL2CPP/external/baselib/Include" -I"$IL2CPP/external/baselib/Platforms/Android/Include" \
 -Iport-deps/sources/fmt/include -Iport-deps/sources/utfcpp/source \
 ports/scotland2/tests/android-customtypes.cpp -L"$OUT" -lcustom-types \
 -Lbuild/scotland2/android-aot -lsl2_abi -L"$OUT/deps" -lbeatsaber-hook -lpaper2_scotland2 \
 -Wl,-z,max-page-size=16384 -Wl,-soname,libsc2_customtypes_probe.so -o "$OUT/libsc2_customtypes_probe.so"
fi
python3 - "$OUT" <<'PY'
from pathlib import Path
import hashlib,json,sys
sys.path.insert(0,'tools');from audit_elf import Elf
out=Path(sys.argv[1]);binary=out/'libcustom-types.so';elf=Elf(str(binary))
lock=json.loads(Path('port-results/coremods/coremods-lock.json').read_text())
original,=[b for b in lock['binaries'] if b['mod']=='custom-types']
released=Elf(str(Path('port-results/coremods')/original['file']))
missing=sorted(x for x in released.exports-elf.exports if 'custom_types' in x)
assert not missing,missing
imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mprotect','mmap'} & imports
assert not any(x.startswith('A64') for x in elf.exports)
assert elf.p_align>=16384 and 'libsl2_abi.so' in elf.needed
record={'source':json.loads(Path('ports/scotland2/customtypes/provenance.json').read_text()),
 'libil2cpp':json.loads(Path('port-deps/sources/libil2cpp-lock.json').read_text()),
 'submodules':json.loads(Path('port-deps/sources/sdk-submodules-lock.json').read_text()),
 'outputSha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
 'originalCustomTypesExportsPreserved':True,'importsAdaptedAnd64':True,
 'noExecutableMemoryImports':True,'registrationVerified':False,
 'scope':'Actual custom-types source build; original type logic retained, embedded And64 compilation replaced by support DSO import. Not yet registration-tested or packaged.',
 'prelude':'ports/scotland2/src/android-mod-sdk-prelude.hpp',
 'targets':json.loads((out/'targets.json').read_text()),
 'probeSha256':hashlib.sha256((out/'libsc2_customtypes_probe.so').read_bytes()).hexdigest() if __import__('os').environ.get('SC2_BUILD_MODE')!='release' else None}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
echo "Built actual custom-types with imported source-adapted And64: $OUT"
