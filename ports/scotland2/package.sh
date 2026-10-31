#!/bin/bash
# Prepare loader frameworks and an explicitly selected dependency set. Never installs.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-macosx}"
TEST_SET="${SC2_TEST_SET:-tinyxml2}"
if [ "$TEST_SET" = coremods ]; then exec bash ports/scotland2/package-coremods.sh "$PLATFORM"; fi
if [ "$TEST_SET" = songdownloader ]; then exec bash ports/scotland2/package-songdownloader.sh "$PLATFORM"; fi
if [ "$TEST_SET" = metacore ]; then exec bash ports/scotland2/package-metacore.sh "$PLATFORM"; fi
if [ "$TEST_SET" = songcore ]; then exec bash ports/scotland2/package-songcore.sh "$PLATFORM"; fi
if [ "$TEST_SET" = bsml ]; then exec bash ports/scotland2/package-bsml.sh "$PLATFORM"; fi
if [ "$TEST_SET" = lapiz ]; then exec bash ports/scotland2/package-lapiz.sh "$PLATFORM"; fi
if [ "$TEST_SET" = customtypes ]; then exec bash ports/scotland2/package-customtypes.sh "$PLATFORM"; fi
if [ "$TEST_SET" = hooks ]; then exec bash ports/scotland2/package-hooks.sh "$PLATFORM"; fi
case "$TEST_SET" in tinyxml2|paper) ;; *) echo 'SC2_TEST_SET must be tinyxml2 or paper.' >&2; exit 2 ;; esac
case "$PLATFORM" in
  macosx) TARGET=arm64-apple-macos14.0; KLPLATFORM=macos; MINOS=14.0 ;;
  xros) TARGET=arm64-apple-xros26.0; KLPLATFORM=visionos; MINOS=26.0 ;;
  *) echo 'Use macosx or xros.' >&2; exit 2 ;;
esac
bash ports/scotland2/build.sh "$PLATFORM"
make build/klepton-ld
OUT="build/scotland2/$PLATFORM"
CORE_OUT="$OUT"
if [ "$TEST_SET" = paper ]; then OUT="$OUT-paper"; fi
BUNDLE="$OUT/bundle"
FW="$BUNDLE/Frameworks/Scotland2Core.framework"
XML="$BUNDLE/Frameworks/libtinyxml2.framework"
META="$BUNDLE/Scotland2Metadata"
mkdir -p "$FW" "$XML" "$META/"{libs,early_mods,mods,shims}
for PHASE in libs early_mods mods shims; do : > "$META/$PHASE/.keep"; done
SDK=$(xcrun --sdk "$PLATFORM" --show-sdk-path)
xcrun clang++ -target "$TARGET" -isysroot "$SDK" -dynamiclib \
  -Wl,-force_load,"$CORE_OUT/libscotland2-core.a" \
  -install_name @rpath/Scotland2Core.framework/Scotland2Core -o "$FW/Scotland2Core"
python3 - "$META" "$TEST_SET" <<'PY'
import hashlib,json,shutil,sys
from pathlib import Path
root = Path('port-results/coremods')
lock = json.loads((root/'coremods-lock.json').read_text())
dest = Path(sys.argv[1])
mods = {'tinyxml2'} | ({'paper2_scotland2'} if sys.argv[2]=='paper' else set())
binaries = [b for b in lock['binaries'] if b['mod'] in mods]
assert len(binaries)==len(mods)
for binary in binaries:
 source = root/binary['file']
 assert hashlib.sha256(source.read_bytes()).hexdigest()==binary['sha256']
 shutil.copy2(source,dest/'libs'/source.name)
expected = {'libs/'+Path(b['file']).name for b in binaries}
if {str(p.relative_to(dest)) for p in dest.rglob('*.so')} != expected:
 raise SystemExit('Unexpected mod metadata in generated test bundle.')
(dest/'provenance.json').write_text(json.dumps({'scotland2':'432f67bca61e822a87b27c001bddcd1061d25bf4','libraries':binaries,
 'testSet':sys.argv[2], 'scope':'Native Apple loader core and selected dependencies. No early/late mods or scene hook.'},indent=2)+'\n')
PY
./build/klepton-ld "$META/libs/libtinyxml2.so" --platform "$KLPLATFORM" \
  --install-name @rpath/libtinyxml2.framework/libtinyxml2 -o "$XML/libtinyxml2" --quiet
python3 - "$FW" "$XML" "$MINOS" <<'PY'
import plistlib,sys
from pathlib import Path
for folder in sys.argv[1:3]:
 path=Path(folder);name=path.stem
 data={'CFBundleExecutable':name,'CFBundleIdentifier':'dev.klepton.scotland2.'+name,
       'CFBundleName':name,'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0',
       'CFBundleVersion':'1','MinimumOSVersion':sys.argv[3]}
 (path/'Info.plist').write_bytes(plistlib.dumps(data))
PY
if [ "$TEST_SET" = paper ]; then
  PAPER="$BUNDLE/Frameworks/libpaper2_scotland2.framework"
  # A previous Mac experiment used a bare dylib, which takes lookup precedence.
  rm -f "$BUNDLE/Frameworks/libpaper2_scotland2.dylib"
  mkdir -p "$PAPER"
  ./build/klepton-ld "$META/libs/libpaper2_scotland2.so" --platform "$KLPLATFORM" \
    --install-name @rpath/libpaper2_scotland2.framework/libpaper2_scotland2 \
    -o "$PAPER/libpaper2_scotland2" --quiet
  python3 - "$PAPER" "$XML" <<'PY'
import plistlib,sys
from pathlib import Path
paper,xml=map(Path,sys.argv[1:])
data=plistlib.loads((xml/'Info.plist').read_bytes())
data.update(CFBundleExecutable=paper.stem,CFBundleName=paper.stem,CFBundleIdentifier='dev.klepton.scotland2.'+paper.stem.replace('_','-'))
(paper/'Info.plist').write_bytes(plistlib.dumps(data))
PY
fi
if [ "$PLATFORM" = macosx ]; then
  codesign --force --sign - "$FW"
  codesign --force --sign - "$XML"
  if [ "$TEST_SET" = paper ]; then codesign --force --sign - "$PAPER"; fi
else
  mkdir -p visionos/Frameworks
  for ITEM in Scotland2Core SC2_libtinyxml2; do
    case "$ITEM" in Scotland2Core) SOURCE="$FW" ;; *) SOURCE="$XML" ;; esac
    rm -rf "visionos/Frameworks/$ITEM.xcframework"
    xcodebuild -create-xcframework -framework "$SOURCE" -output "visionos/Frameworks/$ITEM.xcframework" >/dev/null
  done
  if [ "$TEST_SET" = paper ]; then
    rm -rf visionos/Frameworks/SC2_libpaper2_scotland2.xcframework
    xcodebuild -create-xcframework -framework "$PAPER" -output visionos/Frameworks/SC2_libpaper2_scotland2.xcframework >/dev/null
  fi
  rm -rf visionos/Scotland2Metadata
  mkdir -p visionos/Scotland2Metadata
  cp -R "$META/." visionos/Scotland2Metadata/
fi
echo "Scotland2 test bundle: $BUNDLE"
