#!/bin/bash
# Add actual Lapiz to the existing custom-types test composition.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
SC2_TEST_SET=customtypes bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-lapiz.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-lapiz/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-customtypes-device/bundle/." "$BUNDLE/"
SOURCE="$PWD/build/scotland2/android-lapiz/liblapiz.so"
cp "$SOURCE" "$BUNDLE/Scotland2Metadata/early_mods/"
mkdir -p "$BUNDLE/Scotland2Metadata/profiles"
cp build/scotland2/android-lapiz/hook-profile.txt "$BUNDLE/Scotland2Metadata/profiles/lapiz-hooks.txt"
FRAMEWORK="$BUNDLE/Frameworks/liblapiz.framework"
mkdir -p "$FRAMEWORK"
./build/klepton-ld "$SOURCE" --platform "$KLPLATFORM" --install-name @rpath/liblapiz.framework/liblapiz \
  -o "$FRAMEWORK/liblapiz" --quiet
python3 - "$FRAMEWORK" "$MINOS" "$BUNDLE/Scotland2Metadata" <<'PY'
from pathlib import Path
import json,plistlib,sys
p=Path(sys.argv[1]);name=p.stem
(p/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,
 'CFBundleIdentifier':'dev.klepton.scotland2.'+name,'CFBundleName':name,'CFBundlePackageType':'FMWK',
 'CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':sys.argv[2]}))
meta=Path(sys.argv[3]);p=meta/'provenance.json';d=json.loads(p.read_text());d['testSet']='lapiz'
d['lapiz']=json.loads(Path('build/scotland2/android-lapiz/provenance.json').read_text())
d['scope']='Actual Lapiz source with original early callback and game hooks. No BSML/SongCore or custom-song playback yet.'
p.write_text(json.dumps(d,indent=2)+'\n')
PY
if [ "$PLATFORM" = xros ]; then
  rm -rf visionos/Frameworks/SC2_liblapiz.xcframework
  xcodebuild -create-xcframework -framework "$FRAMEWORK" -output visionos/Frameworks/SC2_liblapiz.xcframework >/dev/null
  rm -rf visionos/Scotland2Metadata
  cp -R "$BUNDLE/Scotland2Metadata" visionos/Scotland2Metadata
else
  codesign --force --sign - "$FRAMEWORK"
fi
echo "Actual Lapiz test bundle: $BUNDLE"
