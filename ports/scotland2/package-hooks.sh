#!/bin/bash
# Package actual dependencies and the two source hook adapters. Never installs.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
SC2_TEST_SET=paper bash ports/scotland2/package.sh "$PLATFORM"
SC2_ANDROID_AOT=1 SC2_ANDROID_CAPSTONE=1 bash ports/scotland2/build-android-paths.sh
bash ports/scotland2/build-flamingo.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-hooks/bundle"
mkdir -p "$BUNDLE"
cp -R "$PWD/build/scotland2/$PLATFORM-paper/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
python3 - "$META" <<'PY'
from pathlib import Path
import hashlib,json,shutil,sys
root=Path('port-results/coremods');meta=Path(sys.argv[1])
lock=json.loads((root/'coremods-lock.json').read_text())
binary,=[b for b in lock['binaries'] if b['mod']=='beatsaber-hook']
source=root/binary['file']
assert hashlib.sha256(source.read_bytes()).hexdigest()==binary['sha256']
shutil.copy2(source,meta/'libs/libbeatsaber-hook.so')
for name,folder in [('libsl2_abi','android-aot'),('libsc2_flamingo','android-flamingo'),('libsc2_flamingo_probe','android-flamingo')]:
 shutil.copy2(Path('build/scotland2')/folder/(name+'.so'),meta/'shims'/(name+'.so'))
expected={'libs/libtinyxml2.so','libs/libpaper2_scotland2.so','libs/libbeatsaber-hook.so',
 'shims/libsl2_abi.so','shims/libsc2_flamingo.so','shims/libsc2_flamingo_probe.so'}
assert {str(p.relative_to(meta)) for p in meta.rglob('*.so')}==expected
record=json.loads((meta/'provenance.json').read_text());record['libraries'].append(binary)
record['testSet']='hooks'
record['scope']='Actual loader/dependencies, source And64/Flamingo adapters and original scene timing. No real early/late mods; custom songs not yet working.'
record['androidSupport']=json.loads(Path('build/scotland2/android-aot/provenance.json').read_text())
record['flamingo']=json.loads(Path('build/scotland2/android-flamingo/provenance.json').read_text())
(meta/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
for ITEM in libbeatsaber-hook libsl2_abi libcxx_shared libsc2_flamingo libsc2_flamingo_probe; do
  case "$ITEM" in
    libbeatsaber-hook) SOURCE="$META/libs/libbeatsaber-hook.so" ;;
    libsl2_abi) SOURCE="$META/shims/libsl2_abi.so" ;;
    libcxx_shared) SOURCE="$PWD/build/scotland2/android-aot/libc++_shared.so" ;;
    *) SOURCE="$META/shims/$ITEM.so" ;;
  esac
  FRAMEWORK="$BUNDLE/Frameworks/$ITEM.framework"
  mkdir -p "$FRAMEWORK"
  ./build/klepton-ld "$SOURCE" --platform "$KLPLATFORM" \
    --install-name "@rpath/$ITEM.framework/$ITEM" -o "$FRAMEWORK/$ITEM" --quiet
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
    rm -rf "visionos/Frameworks/SC2_$ITEM.xcframework"
    xcodebuild -create-xcframework -framework "$FRAMEWORK" -output "visionos/Frameworks/SC2_$ITEM.xcframework" >/dev/null
  else
    codesign --force --sign - "$FRAMEWORK"
  fi
done
if [ "$PLATFORM" = xros ]; then
  rm -rf visionos/Scotland2Metadata
  mkdir -p visionos/Scotland2Metadata
  cp -R "$META/." visionos/Scotland2Metadata/
fi
echo "Hook dependency bundle: $BUNDLE"
