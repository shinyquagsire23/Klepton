#!/bin/bash
# Replace only the test project's generated game frameworks, before signing.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="$PWD/build/scotland2/xros-hooks/game"
mkdir -p "$OUT"
for NAME in libil2cpp libunity; do
  SOURCE="$PWD/beatsaber140/lib/arm64-v8a/$NAME.so"
  FRAMEWORK="$OUT/$NAME.framework"
  mkdir -p "$FRAMEWORK"
  ./build/klepton-ld "$SOURCE" --platform visionos --install-name "@rpath/$NAME.framework/$NAME" \
    -o "$OUT/$NAME.unprepared.dylib" --quiet
  if [ "$NAME" = libil2cpp ]; then
    python3 ports/scotland2/tools/hook_manifest.py --elf "$SOURCE" \
      --translation "$OUT/$NAME.unprepared.dylib" --output "$OUT/$NAME.manifest.json" il2cpp_domain_get
  else
    python3 ports/scotland2/tools/find_scene_target.py --elf "$SOURCE" --output "$OUT/unity-scene-profile.json"
    python3 - "$SOURCE" "$OUT" <<'PY'
from pathlib import Path
import hashlib,json,sys
source=Path(sys.argv[1]);out=Path(sys.argv[2])
profile=json.loads((out/'unity-scene-profile.json').read_text())
assert profile['elf_sha256']==hashlib.sha256(source.read_bytes()).hexdigest()
(out/'libunity.manifest.json').write_text(json.dumps({'schema':1,'elf_sha256':profile['elf_sha256'],
 'translation_sha256':hashlib.sha256((out/'libunity.unprepared.dylib').read_bytes()).hexdigest(),
 'targets':[profile['target']]},indent=2)+'\n')
PY
  fi
  if [[ "${SC2_TEST_SET:-hooks}" = customtypes || "${SC2_TEST_SET:-hooks}" = lapiz || "${SC2_TEST_SET:-hooks}" = bsml || "${SC2_TEST_SET:-hooks}" = songcore || "${SC2_TEST_SET:-hooks}" = metacore || "${SC2_TEST_SET:-hooks}" = songdownloader || "${SC2_TEST_SET:-hooks}" = coremods ]]; then
    python3 - "$NAME" "$SOURCE" "$OUT" <<'PYCT'
from pathlib import Path
import hashlib,json,sys
name=sys.argv[1];source=Path(sys.argv[2]);out=Path(sys.argv[3])
profile=json.loads(Path('build/scotland2/android-customtypes/targets.json').read_text())['il2cpp' if name=='libil2cpp' else 'unity']
assert profile['sha256']==hashlib.sha256(source.read_bytes()).hexdigest()
p=out/(name+'.manifest.json');d=json.loads(p.read_text());d['targets']+=profile['targets']
import os
from collections import Counter
mods={'lapiz':['lapiz'],'bsml':['lapiz','bsml'],'songcore':['lapiz','bsml','songcore'],'metacore':['lapiz','bsml','songcore','metacore'],'songdownloader':['lapiz','bsml','songcore','metacore','playlistcore'],'coremods':['lapiz','bsml','songcore','metacore','playlistcore','datakeeper','unicode']}.get(os.environ.get('SC2_TEST_SET'),[])
if os.environ.get('SC2_TEST_SET')=='coremods':
 mods=json.loads(Path('ports/scotland2/selected-mods.json').read_text())['hookCatalogs']
if os.environ.get('SC2_BUILD_MODE')=='release' and name=='libil2cpp':
 d['targets']=[t for t in d['targets'] if t.get('name')!='il2cpp_domain_get']
original_counts=Counter()
for mod in mods if name=='libil2cpp' else []:
 catalog=json.loads(Path('build/scotland2/android-'+mod+'/hook-catalog.json').read_text())
 assert catalog['gameElfSha256']==d['elf_sha256']
 targets={int(t['vaddr'],0):t for t in d['targets']}
 for target in catalog['targets']:
  old=targets.get(int(target['vaddr'],0))
  assert old is None or int(old['expect'],0)==int(target['expect'],0)
  targets[int(target['vaddr'],0)]=target
 for row in catalog['catalog']:
  if row['originalHook']:original_counts[row['vaddr']]+=1
 d['targets']=list(targets.values());d['originalEntryDepth']=max(d.get('originalEntryDepth',0),catalog['originalEntryDepth'],max(original_counts.values(),default=0))
if name=='libunity' and 'metacore' in mods:
 extra=json.loads(Path('build/scotland2/android-metacore/runtime-targets.json').read_text())['unity']
 assert extra['sha256']==d['elf_sha256']
 d['targets']+=extra['targets']
p.write_text(json.dumps(d,indent=2)+'\n')
PYCT
  fi
  python3 ports/scotland2/tools/prepare_hooks.py --elf "$SOURCE" --input "$OUT/$NAME.unprepared.dylib" \
    --manifest "$OUT/$NAME.manifest.json" --output "$FRAMEWORK/$NAME"
  python3 - "$FRAMEWORK" <<'PY'
from pathlib import Path
import plistlib,sys
p=Path(sys.argv[1]);name=p.stem
(p/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,
 'CFBundleIdentifier':'dev.klepton.scotland2.game.'+name,'CFBundleName':name,
 'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':'26.0'}))
PY
  rm -rf "visionos/Frameworks/beatsaber140/$NAME.xcframework"
  xcodebuild -create-xcframework -framework "$FRAMEWORK" -output "visionos/Frameworks/beatsaber140/$NAME.xcframework" >/dev/null
done
