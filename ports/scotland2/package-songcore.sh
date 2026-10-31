#!/bin/bash
# Actual SongCore in Scotland2's original late phase, with the actual dependencies.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
SC2_TEST_SET=bsml bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-songcore.sh
bash ports/scotland2/build-songcore-probe.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-songcore-device/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-bsml-device/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
cp build/scotland2/android-songcore/libsongcore.so "$META/mods/"
cp build/scotland2/android-songcore/libsc2_songcore_probe.so "$META/shims/"
cp build/scotland2/android-songcore/hook-profile.txt "$META/profiles/songcore-hooks.txt"
for NAME in libsongcore libsc2_songcore_probe; do
 SOURCE="$PWD/build/scotland2/android-songcore/$NAME.so"
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
python3 - "$META" <<'PYMETA'
from pathlib import Path
import json,sys,hashlib
meta=Path(sys.argv[1]);p=meta/'provenance.json';d=json.loads(p.read_text());d['testSet']='songcore'
d['songcore']=json.loads(Path('build/scotland2/android-songcore/provenance.json').read_text())
d['songcoreLifecycleProbeSha256']=hashlib.sha256((meta/'shims/libsc2_songcore_probe.so').read_bytes()).hexdigest()
d['scope']='Actual SongCore in original Scotland2 late phase with Lapiz/BSML, hooks/types/service and scan observation. Song playback not yet verified.'
p.write_text(json.dumps(d,indent=2)+'\n')
PYMETA
if [ "$PLATFORM" = xros ]; then
 rm -rf visionos/Scotland2Metadata
 cp -R "$META" visionos/Scotland2Metadata
fi
echo "Actual SongCore late-callback test bundle: $BUNDLE"
