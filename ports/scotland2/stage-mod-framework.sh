#!/bin/bash
# Translate a selected Android DSO before Apple signing; does not install apps.
set -euo pipefail
cd "$(dirname "$0")/../.."
PLATFORM="$1";SOURCE="$2";DEST="$3"
NAME=$(basename "$SOURCE" .so)
[[ "$NAME" =~ ^lib[A-Za-z0-9_-]+$ ]] || exit 2
case "$PLATFORM" in xros) KLPLATFORM=visionos;MINOS=26.0 ;; macosx) KLPLATFORM=macos;MINOS=14.0 ;; *) exit 2 ;; esac
FRAMEWORK="$DEST/$NAME.framework"
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
