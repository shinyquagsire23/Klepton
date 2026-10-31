#!/bin/bash
# Original in-game SongDownloader, with its real ported dependency stack.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
SC2_TEST_SET=metacore bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-playlistcore.sh
bash ports/scotland2/build-songdownloader.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-songdownloader-device/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-metacore-device/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
for NAME in playlistcore songdownloader; do
 cp "build/scotland2/android-$NAME/lib$NAME.so" "$META/mods/"
 bash ports/scotland2/stage-mod-framework.sh "$PLATFORM" "$META/mods/lib$NAME.so" "$BUNDLE/Frameworks"
done
cp build/scotland2/android-playlistcore/hook-profile.txt "$META/profiles/playlistcore-hooks.txt"
python3 - "$META" <<'PY'
from pathlib import Path
import json,sys
meta=Path(sys.argv[1]);p=meta/'provenance.json';d=json.loads(p.read_text());d['testSet']='songdownloader'
for name in ['playlistcore','songdownloader']:d[name]=json.loads(Path('build/scotland2/android-'+name+'/provenance.json').read_text())
d['scope']='Actual original SongDownloader/PlaylistCore UI and networking with real Scotland2 dependencies. In-game browsing/download/playback require physical verification.'
p.write_text(json.dumps(d,indent=2)+'\n')
PY
if [ "$PLATFORM" = xros ]; then
 rm -rf visionos/Scotland2Metadata
 cp -R "$META" visionos/Scotland2Metadata
fi
echo "Actual SongDownloader test bundle: $BUNDLE"
