#!/bin/bash
# Add actual rebuilt custom-types and its separate registration-test consumer.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
SC2_TEST_SET=hooks bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-customtypes.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-customtypes-device/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-hooks/bundle/." "$BUNDLE/"
for NAME in libcustom-types libsc2_customtypes_probe; do
  PHASE=shims
  if [ "$NAME" = libcustom-types ]; then PHASE=libs; fi
  SOURCE="$PWD/build/scotland2/android-customtypes/$NAME.so"
  cp "$SOURCE" "$BUNDLE/Scotland2Metadata/$PHASE/"
  FRAMEWORK="$BUNDLE/Frameworks/$NAME.framework"
  mkdir -p "$FRAMEWORK"
  ./build/klepton-ld "$SOURCE" --platform "$KLPLATFORM" --install-name "@rpath/$NAME.framework/$NAME" \
    -o "$FRAMEWORK/$NAME" --quiet
  python3 - "$FRAMEWORK" "$MINOS" <<'PY'
from pathlib import Path
import plistlib,sys
p=Path(sys.argv[1]);name=p.stem
(p/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,
 'CFBundleIdentifier':'dev.klepton.scotland2.'+name.replace('_','-'),
 'CFBundleName':name,'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0',
 'CFBundleVersion':'1','MinimumOSVersion':sys.argv[2]}))
PY
  if [ "$PLATFORM" = xros ]; then
    rm -rf "visionos/Frameworks/SC2_$NAME.xcframework"
    xcodebuild -create-xcframework -framework "$FRAMEWORK" -output "visionos/Frameworks/SC2_$NAME.xcframework" >/dev/null
  else
    codesign --force --sign - "$FRAMEWORK"
  fi
done
python3 - "$BUNDLE/Scotland2Metadata" <<'PY'
from pathlib import Path
import json,sys
meta=Path(sys.argv[1]);p=meta/'provenance.json';d=json.loads(p.read_text());d['testSet']='customtypes'
d['customTypes']=json.loads(Path('build/scotland2/android-customtypes/provenance.json').read_text())
d['scope']='Actual custom-types source port with original built-in type registration test at Scotland2 late scene phase; no SongCore or custom-song playback yet.'
p.write_text(json.dumps(d,indent=2)+'\n')
PY
if [ "$PLATFORM" = xros ]; then
  rm -rf visionos/Scotland2Metadata
  cp -R "$BUNDLE/Scotland2Metadata" visionos/Scotland2Metadata
fi
echo "Custom-types registration bundle: $BUNDLE"
