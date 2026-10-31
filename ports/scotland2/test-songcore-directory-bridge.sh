#!/bin/bash
# Requires the existing macOS Scotland2/SongCore host composition. Exercises the
# original Android SongCore collector, including its real NDK filesystem types.
set -euo pipefail
cd "$(dirname "$0")/../.."
bash ports/scotland2/build-songcore-collect-probe.sh
make build/t_scotland2_il2cpp
FRAMEWORKS="$PWD/build/scotland2/macosx-bsml/bundle/Frameworks"
./build/klepton-ld build/scotland2/android-songcore/libsc2_songcore_collect.so \
  --platform macos -o "$FRAMEWORKS/libsc2_songcore_collect.dylib" --quiet
codesign --force --sign - "$FRAMEWORKS/libsc2_songcore_collect.dylib"
FIXTURE="$PWD/build/scotland2/android-filesystem"
python3 - "$FIXTURE" <<'PY'
from pathlib import Path
from zipfile import ZipFile, ZIP_STORED
import sys
p=Path(sys.argv[1]);(p/'obb').mkdir(parents=True,exist_ok=True)
for name in ('song-one','song-two','song-two/autosaves','song-two/autosaves/nested'):
    d=p/'fixture'/name;d.mkdir(parents=True,exist_ok=True)
    (d/'Info.dat').write_text('{}');(d/'song.egg').write_text('diagnostic fixture')
with ZipFile(p/'obb'/'fixture.obb','w',compression=ZIP_STORED) as z:
    z.writestr('Test/Content/Paks/fixture.pak',b'virtual fixture')
PY
mkdir -p build/scotland2/filesystem-userdata
export SC2_FILESYSTEM_PROBE="$PWD/build/scotland2/android-songcore/libsc2_songcore_collect.so"
export SC2_FILESYSTEM_ROOT="$FIXTURE/fixture" SC2_FS_OBBDIR="$FIXTURE/obb"
export KL_SCOTLAND2=1 KL_SCOTLAND2_ABI=1 KL_SCOTLAND2_FILESYSTEM=1
export KL_SCOTLAND2_NATIVE_GATES=1 KL_GUEST_JIT=0 KL_DYLIB_DIR="$FRAMEWORKS"
export KL_FILES_DIR="$PWD/build/scotland2/filesystem-userdata"
unset SC2_FS_UE_BASE SC2_FS_UE_DIRECTORY
SC2_FS_UNITY_GUARD=1 ./build/t_scotland2_il2cpp \
  "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" "$PWD/beatsaber140/assets/bin/Data/Managed"
SC2_FS_UE_BASE="$FIXTURE/ue-base" SC2_FS_UE_DIRECTORY="$FIXTURE/ue-base/Test/Content/Paks" \
  ./build/t_scotland2_il2cpp "$PWD/beatsaber140/lib/arm64-v8a/libil2cpp.so" \
  "$PWD/beatsaber140/assets/bin/Data/Managed"
