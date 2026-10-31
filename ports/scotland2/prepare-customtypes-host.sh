#!/bin/bash
# Host test composition only; exact original-source targets, before signing.
set -euo pipefail
cd "$(dirname "$0")/../.."
FRAMEWORKS="$1"
PROFILE="$PWD/build/scotland2/android-customtypes/targets.json"
for NAME in libil2cpp libunity libmain; do
  SOURCE="$PWD/beatsaber140/lib/arm64-v8a/$NAME.so"
  ./build/klepton-ld "$SOURCE" --platform macos -o "$FRAMEWORKS/$NAME.unprepared.dylib" --quiet
  if [ "$NAME" = libmain ]; then
    cp "$FRAMEWORKS/$NAME.unprepared.dylib" "$FRAMEWORKS/$NAME.dylib"
  else
    python3 - "$PROFILE" "$NAME" "$SOURCE" "$FRAMEWORKS" <<'PY'
from pathlib import Path
import hashlib,json,sys
profile=json.loads(Path(sys.argv[1]).read_text());name=sys.argv[2];raw=Path(sys.argv[3]);out=Path(sys.argv[4])
row=profile['il2cpp' if name=='libil2cpp' else 'unity']
assert hashlib.sha256(raw.read_bytes()).hexdigest()==row['sha256']
manifest={'schema':1,'elf_sha256':row['sha256'],'translation_sha256':hashlib.sha256((out/(name+'.unprepared.dylib')).read_bytes()).hexdigest(),'targets':row['targets']}
import os
for variable in ['SC2_LAPIZ_CATALOG','SC2_BSML_CATALOG','SC2_METACORE_CATALOG','SC2_PLAYLISTCORE_CATALOG','SC2_DATAKEEPER_CATALOG','SC2_FAKEID_CATALOG','SC2_UNICODE_CATALOG']:
  if os.environ.get(variable) and name=='libil2cpp':
    catalog=json.loads(Path(os.environ[variable]).read_text())
    assert catalog['gameElfSha256']==row['sha256']
    targets={int(t['vaddr'],0):t for t in manifest['targets']}
    for target in catalog['targets']:
        old=targets.get(int(target['vaddr'],0))
        assert old is None or int(old['expect'],0)==int(target['expect'],0)
        targets[int(target['vaddr'],0)]=target
    manifest['targets']=list(targets.values())
    manifest['originalEntryDepth']=max(manifest.get('originalEntryDepth',0),catalog['originalEntryDepth'])
if os.environ.get('SC2_METACORE_RUNTIME_PROFILE') and name=='libunity':
    extra=json.loads(Path(os.environ['SC2_METACORE_RUNTIME_PROFILE']).read_text())['unity']
    assert extra['sha256']==manifest['elf_sha256']
    manifest['targets']+=extra['targets']
(out/(name+'.manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
PY
    python3 ports/scotland2/tools/prepare_hooks.py --elf "$SOURCE" --input "$FRAMEWORKS/$NAME.unprepared.dylib" \
      --manifest "$FRAMEWORKS/$NAME.manifest.json" --output "$FRAMEWORKS/$NAME.dylib"
  fi
  codesign --force --sign - "$FRAMEWORKS/$NAME.dylib"
done
