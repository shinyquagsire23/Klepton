#!/bin/bash
# Mac-only signed guest ABI integration test; optional signed hook fixtures.
set -euo pipefail
cd "$(dirname "$0")/../.."
ANDROID_OUT=build/scotland2/android-paths
export SC2_ANDROID_CAPSTONE=0 SC2_ANDROID_AOT=0
if [ "${SC2_TEST_FLAMINGO:-0}" = 1 ]; then export SC2_TEST_AOT=1; fi
if [ "${SC2_TEST_SDK:-0}" = 1 ]; then export SC2_TEST_AOT=1; fi
if [ "${SC2_TEST_AOT:-0}" = 1 ]; then export SC2_TEST_BSHOOK=1; fi
if [ "${SC2_TEST_BSHOOK:-0}" = 1 ]; then
  export SC2_ANDROID_CAPSTONE=1
  ANDROID_OUT=build/scotland2/android-compat
fi
if [ "${SC2_TEST_AOT:-0}" = 1 ]; then export SC2_ANDROID_AOT=1; ANDROID_OUT=build/scotland2/android-aot; fi
bash ports/scotland2/build-android-paths.sh
SC2_TEST_SET=paper bash ports/scotland2/package.sh macosx
BUNDLE="$PWD/build/scotland2/macosx-abi/bundle"
if [ "${SC2_TEST_BSHOOK:-0}" = 1 ]; then BUNDLE="$PWD/build/scotland2/macosx-bshook/bundle"; fi
if [ "${SC2_TEST_AOT:-0}" = 1 ]; then BUNDLE="$PWD/build/scotland2/macosx-aot/bundle"; fi
if [ "${SC2_TEST_FLAMINGO:-0}" = 1 ]; then BUNDLE="$PWD/build/scotland2/macosx-flamingo/bundle"; fi
if [ "${SC2_TEST_SDK:-0}" = 1 ]; then BUNDLE="$PWD/build/scotland2/macosx-sdk-hooks/bundle"; fi
mkdir -p "$BUNDLE"
cp -R "$PWD/build/scotland2/macosx-paper/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
FRAMEWORKS="$BUNDLE/Frameworks"
cp "$ANDROID_OUT/libsl2_abi.so" "$META/shims/"
mkdir -p "$META/test-only"
cp "$ANDROID_OUT/libsc2_abi_consumer.so" "$META/test-only/"
for ITEM in libsl2_abi libsc2_abi_consumer libc++_shared; do
  FSNAME="${ITEM//+/x}"
  ./build/klepton-ld "$ANDROID_OUT/$ITEM.so" --platform macos \
    --install-name "@rpath/$FSNAME.dylib" -o "$FRAMEWORKS/$FSNAME.dylib" --quiet
  codesign --force --sign - "$FRAMEWORKS/$FSNAME.dylib"
done
rm -f "$FRAMEWORKS/libc++_shared.dylib"
cp "$ANDROID_OUT/provenance.json" "$BUNDLE/android-paths-provenance.json"
if [ "${SC2_TEST_AOT:-0}" = 1 ]; then
  bash ports/scotland2/test-aot-hooks.sh
  cp build/scotland2/aot-hooks/libsc2_hook_fixture.so "$META/test-only/"
  cp build/scotland2/aot-hooks/libsc2_hook_fixture.dylib "$FRAMEWORKS/"
fi
if [ "${SC2_TEST_FLAMINGO:-0}" = 1 ]; then
  bash ports/scotland2/build-flamingo.sh
  for ITEM in libsc2_flamingo libsc2_flamingo_probe; do
    cp "build/scotland2/android-flamingo/$ITEM.so" "$META/shims/"
    ./build/klepton-ld "build/scotland2/android-flamingo/$ITEM.so" --platform macos \
      -o "$FRAMEWORKS/$ITEM.dylib" --quiet
    codesign --force --sign - "$FRAMEWORKS/$ITEM.dylib"
  done
  cp build/scotland2/android-flamingo/provenance.json "$BUNDLE/flamingo-provenance.json"
fi
if [ "${SC2_TEST_BSHOOK:-0}" = 1 ]; then
  python3 - "$META" <<'PY'
import hashlib,json,shutil,sys
from pathlib import Path
root=Path('port-results/coremods')
lock=json.loads((root/'coremods-lock.json').read_text())
binary,=[b for b in lock['binaries'] if b['mod']=='beatsaber-hook']
source=root/binary['file']
assert hashlib.sha256(source.read_bytes()).hexdigest()==binary['sha256']
dest=Path(sys.argv[1]);shutil.copy2(source,dest/'libs/libbeatsaber-hook.so')
record=json.loads((dest/'provenance.json').read_text())
record['libraries'].append(binary)
record['scope']='Mac-only actual beatsaber-hook initialization/Capstone test. Hook installation remains unsupported and executable-memory restrictions stay enabled.'
(dest/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
  ./build/klepton-ld "$META/libs/libbeatsaber-hook.so" --platform macos \
    -o "$FRAMEWORKS/libbeatsaber-hook.dylib" --quiet
  codesign --force --sign - "$FRAMEWORKS/libbeatsaber-hook.dylib"
fi
if [ "${SC2_TEST_SDK:-0}" = 1 ]; then
  bash ports/scotland2/build-sdk-hooks.sh
  cp build/scotland2/android-sdk-hooks/libsc2_sdk_hooks.so "$META/test-only/"
  ./build/klepton-ld "$META/test-only/libsc2_sdk_hooks.so" --platform macos -o "$FRAMEWORKS/libsc2_sdk_hooks.dylib" --quiet
  codesign --force --sign - "$FRAMEWORKS/libsc2_sdk_hooks.dylib"
fi
python3 - "$META" "${SC2_TEST_BSHOOK:-0}" "${SC2_TEST_AOT:-0}" "${SC2_TEST_FLAMINGO:-0}" "${SC2_TEST_SDK:-0}" <<'PY'
import sys
from pathlib import Path
root=Path(sys.argv[1])
expected={'libs/libtinyxml2.so','libs/libpaper2_scotland2.so',
          'shims/libsl2_abi.so','test-only/libsc2_abi_consumer.so'}
if sys.argv[2]=='1': expected.add('libs/libbeatsaber-hook.so')
if sys.argv[3]=='1': expected.add('test-only/libsc2_hook_fixture.so')
if sys.argv[4]=='1': expected|={'shims/libsc2_flamingo.so','shims/libsc2_flamingo_probe.so'}
if sys.argv[5]=='1': expected.add('test-only/libsc2_sdk_hooks.so')
actual={str(p.relative_to(root)) for p in root.rglob('*.so')}
assert actual==expected,(actual,expected)
PY
make build/t_scotland2_paper
USERDATA=$(mktemp -d "$PWD/build/scotland2/abi-userdata-XXXXXXXX")
KL_SCOTLAND2=1 KL_SCOTLAND2_ABI=1 KL_GUEST_JIT=0 KL_DYLIB_DIR="$FRAMEWORKS" KL_SCOTLAND2_ROOT="$META" \
  KL_SCOTLAND2_ABI_CONSUMER="$META/test-only/libsc2_abi_consumer.so" \
  KL_SCOTLAND2_BSHOOK_TEST="${SC2_TEST_BSHOOK:-0}" \
  KL_SCOTLAND2_AOT_FIXTURE="$META/test-only/libsc2_hook_fixture.so" KL_SCOTLAND2_AOT_TEST="${SC2_TEST_AOT:-0}" \
  KL_SCOTLAND2_FLAMINGO="${SC2_TEST_FLAMINGO:-0}" KL_SCOTLAND2_FLAMINGO_PROBE="${SC2_TEST_FLAMINGO:-0}" \
  KL_SCOTLAND2_SDK_HOOK_PROBE="${SC2_TEST_SDK:-0}" \
  ./build/t_scotland2_paper "$USERDATA"
