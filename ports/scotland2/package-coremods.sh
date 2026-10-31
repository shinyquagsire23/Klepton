#!/bin/bash
# Add PlayerDataKeeper and Unicode; Fake ID is redundant with Klepton.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
SC2_TEST_SET=songdownloader bash ports/scotland2/package.sh "$PLATFORM"
BUNDLE="$PWD/build/scotland2/$PLATFORM-coremods-device/bundle"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE";cp -R "build/scotland2/$PLATFORM-songdownloader-device/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
for name in datakeeper unicode;do
 python3 ports/scotland2/tools/build_remaining_core.py "$name"
 lib=lib$name
 phase=early_mods;if [ "$name" = unicode ];then phase=mods;fi
 cp "build/scotland2/android-$name/$lib.so" "$META/$phase/"
 cp "build/scotland2/android-$name/hook-profile.txt" "$META/profiles/$name-hooks.txt"
 bash ports/scotland2/stage-mod-framework.sh "$PLATFORM" "$META/$phase/$lib.so" "$BUNDLE/Frameworks"
done
python3 - "$META" <<'PY'
from pathlib import Path
import json,sys
p=Path(sys.argv[1])/'provenance.json';d=json.loads(p.read_text());d['testSet']='coremods'
for name in ['datakeeper','unicode']:d[name]=json.loads(Path('build/scotland2/android-'+name+'/provenance.json').read_text())
d.pop('fakeid',None)
d['excludedMods']={'fakeid':'Klepton already supplies the adult age-category response; excluded at user request.'}
d['scope']='Ten selected original core mods built; Fake ID excluded. Physical current-build verification pending.';p.write_text(json.dumps(d,indent=2)+'\n')
PY
if [ "$PLATFORM" = xros ];then rm -rf visionos/Scotland2Metadata;cp -R "$META" visionos/Scotland2Metadata;fi
