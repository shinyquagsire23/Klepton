#!/bin/bash
# Actual BSML and original late Unity callback, over the existing Lapiz test set.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
SC2_TEST_SET=lapiz bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-bsml.sh
bash ports/scotland2/build-bsml-probe.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-bsml-device/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-lapiz/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
cp build/scotland2/android-bsml/libbsml.so "$META/early_mods/"
cp build/scotland2/android-bsml/libsc2_bsml_probe.so "$META/shims/"
cp build/scotland2/android-bsml/hook-profile.txt "$META/profiles/bsml-hooks.txt"
for NAME in libbsml libsc2_bsml_probe; do
 SOURCE="$PWD/build/scotland2/android-bsml/$NAME.so"
 FRAMEWORK="$BUNDLE/Frameworks/$NAME.framework"
 mkdir -p "$FRAMEWORK"
 ./build/klepton-ld "$SOURCE" --platform "$KLPLATFORM" --install-name "@rpath/$NAME.framework/$NAME" -o "$FRAMEWORK/$NAME" --quiet
 python3 - "$FRAMEWORK" "$MINOS" <<'PY'
from pathlib import Path
import plistlib,sys
p=Path(sys.argv[1]);name=p.stem
(p/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,'CFBundleIdentifier':'dev.klepton.scotland2.'+name.replace('_','-'),'CFBundleName':name,'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':sys.argv[2]}))
PY
 if [ "$PLATFORM" = xros ]; then
  rm -rf "visionos/Frameworks/SC2_$NAME.xcframework"
  xcodebuild -create-xcframework -framework "$FRAMEWORK" -output "visionos/Frameworks/SC2_$NAME.xcframework" >/dev/null
 else
  codesign --force --sign - "$FRAMEWORK"
 fi
done
# Upstream mkpath invokes a shell; prepare its signed entry for the direct-filesystem adapter.
FW="$BUNDLE/Frameworks/libbeatsaber-hook.framework"
./build/klepton-ld "$META/libs/libbeatsaber-hook.so" --platform "$KLPLATFORM" \
 --install-name @rpath/libbeatsaber-hook.framework/libbeatsaber-hook -o "$FW/unprepared.dylib" --quiet
python3 ports/scotland2/tools/hook_manifest.py --elf "$META/libs/libbeatsaber-hook.so" \
 --translation "$FW/unprepared.dylib" --output "$FW/manifest.json" _Z6mkpathNSt6__ndk117basic_string_viewIcNS_11char_traitsIcEEEE
python3 ports/scotland2/tools/prepare_hooks.py --elf "$META/libs/libbeatsaber-hook.so" \
 --input "$FW/unprepared.dylib" --manifest "$FW/manifest.json" --output "$FW/libbeatsaber-hook"
rm "$FW/unprepared.dylib" "$FW/manifest.json"
python3 - "$META" <<'PY'
from pathlib import Path
import json,sys,hashlib
meta=Path(sys.argv[1]);p=meta/'provenance.json';d=json.loads(p.read_text());d['testSet']='bsml'
d['bsml']=json.loads(Path('build/scotland2/android-bsml/provenance.json').read_text())
d['bsmlLifecycleProbeSha256']=hashlib.sha256((meta/'shims/libsc2_bsml_probe.so').read_bytes()).hexdigest()
d['scope']='Actual Lapiz/BSML source and original early/late callbacks, signed filesystem helper. No SongCore or custom-song playback yet.'
p.write_text(json.dumps(d,indent=2)+'\n')
PY
if [ "$PLATFORM" = xros ]; then
 rm -rf visionos/Frameworks/SC2_libbeatsaber-hook.xcframework
 xcodebuild -create-xcframework -framework "$FW" -output visionos/Frameworks/SC2_libbeatsaber-hook.xcframework >/dev/null
 rm -rf visionos/Scotland2Metadata
 cp -R "$META" visionos/Scotland2Metadata
else
 codesign --force --sign - "$FW"
fi
echo "Actual BSML lifecycle test bundle: $BUNDLE"
