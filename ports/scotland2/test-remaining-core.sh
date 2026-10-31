#!/bin/bash
# Isolated host composition. Never installs, launches or touches headset data.
set -euo pipefail
cd "$(dirname "$0")/../.."
BUNDLE="$PWD/build/scotland2/macosx-remaining-core/bundle"
python3 - "$BUNDLE" <<'PYCLEAN'
import pathlib,shutil,sys
root=pathlib.Path('build/scotland2').resolve();target=pathlib.Path(sys.argv[1])
if target.is_symlink() or not target.resolve().is_relative_to(root):raise ValueError('test output aliases an external path')
if target.exists():shutil.rmtree(target)
shutil.copytree(root/'macosx-bsml/bundle',target)
PYCLEAN
META="$BUNDLE/Scotland2Metadata";FRAMEWORKS="$BUNDLE/Frameworks"
rm -f "$META/test-only/libFakeId.so" "$META/early_mods/libFakeId.so" "$FRAMEWORKS/libFakeId.dylib"
for name in datakeeper unicode;do
 lib=lib$name
 phase=test-only;if [ "$name" = datakeeper ];then phase=early_mods;fi
 cp "build/scotland2/android-$name/$lib.so" "$META/$phase/"
 ./build/klepton-ld "build/scotland2/android-$name/$lib.so" --platform macos -o "$FRAMEWORKS/$lib.dylib" --quiet
 codesign --force --sign - "$FRAMEWORKS/$lib.dylib"
done
export SC2_LAPIZ_CATALOG="$PWD/build/scotland2/android-lapiz/hook-catalog.json"
export SC2_BSML_CATALOG="$PWD/build/scotland2/android-bsml/hook-catalog.json"
export SC2_DATAKEEPER_CATALOG="$PWD/build/scotland2/android-datakeeper/hook-catalog.json"
unset SC2_FAKEID_CATALOG
export SC2_UNICODE_CATALOG="$PWD/build/scotland2/android-unicode/hook-catalog.json"
export SC2_REMAINING_CORE=1 SC2_REMAINING_META="$META"
# The diagnostic expects the actual per-mod output folders, not a synthetic ABI.
export SC2_REMAINING_BUILD="$PWD/build/scotland2"
export SC2_TYPES_PROBE_PATH="$META/shims/libsc2_customtypes_probe.so"
export KL_SCOTLAND2_NATIVE_GATES=1 KL_SCOTLAND2_ABI=1 KL_SCOTLAND2_FILESYSTEM=1 KL_SCOTLAND2_BSML_PROBE=1
export KL_SCOTLAND2_LAPIZ_PROBE=0 KL_GUEST_JIT=0 SC2_PREPARE_CUSTOMTYPES=1
export KL_SCOTLAND2_CUSTOMTYPES_UNITY="$PWD/beatsaber140/lib/arm64-v8a/libunity.so"
export SC2_USE_PREPARED_BUNDLE="$BUNDLE" SC2_PREPARE_GAME_HOOK=0
bash ports/scotland2/test-il2cpp.sh
