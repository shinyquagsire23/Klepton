#!/bin/bash
# Actual MetaCore plus the existing SongCore composition and real network libs.
set -euo pipefail
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  echo 'This progressive helper is diagnostic only. Use ports/scotland2/tools/build_release.py for the selected release.' >&2
  exit 2
fi
cd "$(dirname "$0")/../.."
PLATFORM="${1:-xros}"
SC2_TEST_SET=songcore bash ports/scotland2/package.sh "$PLATFORM"
bash ports/scotland2/build-webutils.sh
bash ports/scotland2/build-metacore.sh
bash ports/scotland2/build-metacore-probe.sh
BUNDLE="$PWD/build/scotland2/$PLATFORM-metacore-device/bundle"
mkdir -p "$BUNDLE"
cp -R "build/scotland2/$PLATFORM-songcore-device/bundle/." "$BUNDLE/"
META="$BUNDLE/Scotland2Metadata"
python3 - "$META" <<'PY'
from pathlib import Path
import json,hashlib,shutil,sys
meta=Path(sys.argv[1]);root=Path('port-results/coremods');lock=json.loads((root/'coremods-lock.json').read_text());selected=[]
for row in lock['binaries']:
 if row['mod'] not in ('web-utils','beatsaverplusplus','song-details'):continue
 source=root/row['file'];assert hashlib.sha256(source.read_bytes()).hexdigest()==row['sha256']
 shutil.copy2(source,meta/'libs'/source.name);selected.append(row)
assert len(selected)==4
shutil.copy2('build/scotland2/android-webutils/libweb-utils.so',meta/'libs/libweb-utils.so')
cert=Path('ports/scotland2/certificates');trust=json.loads((cert/'provenance.json').read_text())
assert hashlib.sha256((cert/'cacert.pem').read_bytes()).hexdigest()==trust['sha256']
shutil.copytree(cert,meta/'certificates',dirs_exist_ok=True)
p=meta/'provenance.json';d=json.loads(p.read_text());d.update(testSet='metacore',metacore=json.loads(Path('build/scotland2/android-metacore/provenance.json').read_text()),webutils=json.loads(Path('build/scotland2/android-webutils/provenance.json').read_text()),networkLibraries=selected,certificateTrust=trust,scope='Actual MetaCore original late callback and measured Unity destruction probe, with SongCore and standalone HTTPS trust. Physical results pending.')
p.write_text(json.dumps(d,indent=2)+'\n')
PY
cp build/scotland2/android-metacore/libmetacore.so "$META/early_mods/"
cp build/scotland2/android-metacore/libsc2_metacore_probe.so "$META/shims/"
cp build/scotland2/android-metacore/hook-profile.txt "$META/profiles/metacore-hooks.txt"
for NAME in libweb-utils libbeatsaverplusplus libzip libsongdetails; do
 bash ports/scotland2/stage-mod-framework.sh "$PLATFORM" "$META/libs/$NAME.so" "$BUNDLE/Frameworks"
done
bash ports/scotland2/stage-mod-framework.sh "$PLATFORM" "$META/early_mods/libmetacore.so" "$BUNDLE/Frameworks"
bash ports/scotland2/stage-mod-framework.sh "$PLATFORM" "$META/shims/libsc2_metacore_probe.so" "$BUNDLE/Frameworks"
if [ "$PLATFORM" = xros ]; then
 rm -rf visionos/Scotland2Metadata
 cp -R "$META" visionos/Scotland2Metadata
fi
echo "Actual MetaCore test bundle: $BUNDLE"
