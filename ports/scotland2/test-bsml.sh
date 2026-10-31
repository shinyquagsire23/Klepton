#!/bin/bash
# Actual Lapiz + BSML early callbacks only; Unity engine startup requires device.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash ports/scotland2/build-bsml.sh
bash ports/scotland2/build-types-probe.sh
bash ports/scotland2/build-bsml-probe.sh
SC2_ANDROID_AOT=1 SC2_ANDROID_CAPSTONE=1 bash ports/scotland2/build-android-paths.sh
BASE="$PWD/build/scotland2/macosx-lapiz/bundle"
BUNDLE="$PWD/build/scotland2/macosx-bsml/bundle"
test -f "$BASE/Frameworks/Scotland2Core.framework/Scotland2Core"
mkdir -p "$BUNDLE"
cp -R "$BASE/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
rm -f "$META/early_mods/libmetacore.so" "$BUNDLE/Frameworks/libmetacore.dylib"
rm -f "$META/test-only/libplaylistcore.so" "$BUNDLE/Frameworks/libplaylistcore.dylib"
rm -f "$META/test-only/libsongdownloader.so" "$BUNDLE/Frameworks/libsongdownloader.dylib"
# Optional released-library probes must not leak into later compositions.
for NAME in libweb-utils libbeatsaverplusplus libzip libsongdetails; do
  rm -f "$META/libs/$NAME.so" "$BUNDLE/Frameworks/$NAME.dylib"
done
cp build/scotland2/android-bsml/libbsml.so "$META/early_mods/"
cp build/scotland2/android-bsml/libsc2_bsml_probe.so "$META/shims/"
cp build/scotland2/android-bsml/hook-profile.txt "$META/profiles/bsml-hooks.txt"
cp build/scotland2/android-customtypes/libsc2_customtypes_probe.so "$META/shims/"
cp build/scotland2/android-aot/libsl2_abi.so "$META/shims/"
for NAME in libbsml libsc2_bsml_probe libsc2_customtypes_probe; do
  if [[ "$NAME" = libbsml || "$NAME" = libsc2_bsml_probe ]]; then DIR=android-bsml; else DIR=android-customtypes; fi
  ./build/klepton-ld "build/scotland2/$DIR/$NAME.so" --platform macos -o "$BUNDLE/Frameworks/$NAME.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/$NAME.dylib"
done
./build/klepton-ld "$META/shims/libsl2_abi.so" --platform macos -o "$BUNDLE/Frameworks/libsl2_abi.dylib" --quiet
codesign --force --sign - "$BUNDLE/Frameworks/libsl2_abi.dylib"
./build/klepton-ld "$META/libs/libbeatsaber-hook.so" --platform macos -o "$BUNDLE/Frameworks/libbeatsaber-hook.unprepared.dylib" --quiet
python3 ports/scotland2/tools/hook_manifest.py --elf "$META/libs/libbeatsaber-hook.so" \
 --translation "$BUNDLE/Frameworks/libbeatsaber-hook.unprepared.dylib" --output "$BUNDLE/Frameworks/bshook-manifest.json" \
 _Z6mkpathNSt6__ndk117basic_string_viewIcNS_11char_traitsIcEEEE
python3 ports/scotland2/tools/prepare_hooks.py --elf "$META/libs/libbeatsaber-hook.so" \
 --input "$BUNDLE/Frameworks/libbeatsaber-hook.unprepared.dylib" --manifest "$BUNDLE/Frameworks/bshook-manifest.json" \
 --output "$BUNDLE/Frameworks/libbeatsaber-hook.dylib"
codesign --force --sign - "$BUNDLE/Frameworks/libbeatsaber-hook.dylib"
if [ "${SC2_TEST_SONGCORE:-0}" = 1 ]; then
  bash ports/scotland2/build-songcore.sh
  bash ports/scotland2/build-songcore-probe.sh
  cp build/scotland2/android-songcore/libsc2_songcore_probe.so "$META/test-only/"
  ./build/klepton-ld "$META/test-only/libsc2_songcore_probe.so" --platform macos -o "$BUNDLE/Frameworks/libsc2_songcore_probe.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libsc2_songcore_probe.dylib"
  export SC2_SONGCORE_LIFECYCLE_PATH="$META/test-only/libsc2_songcore_probe.so"
  cp build/scotland2/android-songcore/libsongcore.so "$META/test-only/"
  ./build/klepton-ld "$META/test-only/libsongcore.so" --platform macos -o "$BUNDLE/Frameworks/libsongcore.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libsongcore.dylib"
  export SC2_SONGCORE_PATH="$META/test-only/libsongcore.so"
fi
if [ "${SC2_TEST_DOWNLOAD_LIBS:-0}" = 1 ]; then
  python3 - "$META" <<'PYLIBS'
from pathlib import Path
import json,hashlib,shutil,sys
meta=Path(sys.argv[1]);root=Path('port-results/coremods');lock=json.loads((root/'coremods-lock.json').read_text())
selected=[]
for row in lock['binaries']:
 if row['mod'] not in ('web-utils','beatsaverplusplus','song-details'):continue
 source=root/row['file'];assert hashlib.sha256(source.read_bytes()).hexdigest()==row['sha256']
 shutil.copy2(source,meta/'libs'/source.name);selected.append(source.stem)
assert set(selected)=={'libweb-utils','libbeatsaverplusplus','libzip','libsongdetails'}
PYLIBS
  if [ "${SC2_TEST_PORTED_WEBUTILS:-0}" = 1 ]; then
    bash ports/scotland2/build-webutils.sh
    cp build/scotland2/android-webutils/libweb-utils.so "$META/libs/"
    python3 ports/scotland2/tools/build_network_support.py song-details
    cp build/scotland2/android-song-details/libsongdetails.so "$META/libs/"
  fi
  for NAME in libweb-utils libbeatsaverplusplus libzip libsongdetails; do
    ./build/klepton-ld "$META/libs/$NAME.so" --platform macos -o "$BUNDLE/Frameworks/$NAME.dylib" --quiet
    codesign --force --sign - "$BUNDLE/Frameworks/$NAME.dylib"
  done
  python3 ports/scotland2/tools/prepare_library_boundary.py --elf "$META/libs/libbeatsaverplusplus.so" \
    --output "$BUNDLE/Frameworks/libbeatsaverplusplus.dylib" --platform macos \
    --symbol _ZN9BeatSaver5Utils10ExtractAllENSt6__ndk14spanIKhLm18446744073709551615EEERKNS1_4__fs10filesystem4pathE
  codesign --force --sign - "$BUNDLE/Frameworks/libbeatsaverplusplus.dylib"
  export KL_SCOTLAND2_DOWNLOAD_BOUNDARIES=1
  bash ports/scotland2/build-download-probe.sh
  cp build/scotland2/android-download-probe/libsc2_download_probe.so "$META/shims/"
  ./build/klepton-ld "$META/shims/libsc2_download_probe.so" --platform macos -o "$BUNDLE/Frameworks/libsc2_download_probe.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libsc2_download_probe.dylib"
  export SC2_DOWNLOAD_PROBE_PATH="$META/shims/libsc2_download_probe.so"
  export SC2_TEST_ZIP_PATH="$BUNDLE/deflate-test.zip"
  python3 - "$SC2_TEST_ZIP_PATH" <<'PYZIP'
import sys,zipfile
with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_DEFLATED) as z:
 z.writestr('payload.txt',b'SCOTLAND2_REAL_ZIP_DEFLATE\n'*100)
PYZIP
  export SC2_TEST_BAD_ZIP_PATH="$BUNDLE/traversal-test.zip"
  python3 - "$SC2_TEST_BAD_ZIP_PATH" <<'PYBADZIP'
import sys,zipfile
with zipfile.ZipFile(sys.argv[1],'w',compression=zipfile.ZIP_DEFLATED) as z:z.writestr('../escape',b'rejected')
PYBADZIP
  export SC2_DOWNLOAD_LIBRARIES=1
fi
if [ "${SC2_TEST_METACORE:-0}" = 1 ]; then
  bash ports/scotland2/build-metacore.sh
  cp build/scotland2/android-metacore/libmetacore.so "$META/early_mods/"
  ./build/klepton-ld "$META/early_mods/libmetacore.so" --platform macos -o "$BUNDLE/Frameworks/libmetacore.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libmetacore.dylib"
  export KL_SCOTLAND2_NATIVE_GATES=1
  export SC2_METACORE_CATALOG="$PWD/build/scotland2/android-metacore/hook-catalog.json"
  export SC2_METACORE_PROFILE="$PWD/build/scotland2/android-metacore/hook-profile.txt"
  export SC2_METACORE_RUNTIME_PROFILE="$PWD/build/scotland2/android-metacore/runtime-targets.json"
fi
if [ "${SC2_TEST_PLAYLISTCORE:-0}" = 1 ]; then
  bash ports/scotland2/build-playlistcore.sh
  cp build/scotland2/android-playlistcore/libplaylistcore.so "$META/test-only/"
  ./build/klepton-ld "$META/test-only/libplaylistcore.so" --platform macos -o "$BUNDLE/Frameworks/libplaylistcore.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libplaylistcore.dylib"
  export SC2_PLAYLISTCORE_PATH="$META/test-only/libplaylistcore.so"
  export SC2_PLAYLISTCORE_CATALOG="$PWD/build/scotland2/android-playlistcore/hook-catalog.json"
  export SC2_PLAYLISTCORE_PROFILE="$PWD/build/scotland2/android-playlistcore/hook-profile.txt"
fi
if [ "${SC2_TEST_SONGDOWNLOADER:-0}" = 1 ]; then
  bash ports/scotland2/build-songdownloader.sh
  cp build/scotland2/android-songdownloader/libsongdownloader.so "$META/test-only/"
  ./build/klepton-ld "$META/test-only/libsongdownloader.so" --platform macos -o "$BUNDLE/Frameworks/libsongdownloader.dylib" --quiet
  codesign --force --sign - "$BUNDLE/Frameworks/libsongdownloader.dylib"
  export SC2_SONGDOWNLOADER_PATH="$META/test-only/libsongdownloader.so"
fi
SC2_BSML_LIFECYCLE_PATH="$META/shims/libsc2_bsml_probe.so" KL_SCOTLAND2_BSML_PROBE=1 KL_SCOTLAND2_ABI=1 KL_SCOTLAND2_FILESYSTEM=1 KL_SCOTLAND2_LAPIZ_PROBE=0 KL_GUEST_JIT=0 SC2_PREPARE_CUSTOMTYPES=1 \
 SC2_LAPIZ_CATALOG="$PWD/build/scotland2/android-lapiz/hook-catalog.json" \
 SC2_BSML_CATALOG="$PWD/build/scotland2/android-bsml/hook-catalog.json" \
 SC2_BSML_PROFILE="$PWD/build/scotland2/android-bsml/hook-profile.txt" \
 SC2_TYPES_PROBE_PATH="$META/shims/libsc2_customtypes_probe.so" \
 KL_SCOTLAND2_CUSTOMTYPES_UNITY="$PWD/beatsaber140/lib/arm64-v8a/libunity.so" \
 SC2_USE_PREPARED_BUNDLE="$BUNDLE" SC2_PREPARE_GAME_HOOK=0 \
 bash ports/scotland2/test-il2cpp.sh
