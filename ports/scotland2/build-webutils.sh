#!/bin/bash
# Actual original WebUtils/curl with explicit CA configuration; no HTTP replacement.
set -euo pipefail
cd "$(dirname "$0")/../.."
NDK="${ANDROID_NDK_HOME:-$PWD/port-deps/toolchains/android-ndk-r27c}"
CC="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++"
REPRO_FLAGS=(-fdebug-compilation-dir=.)
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  REPRO_FLAGS=("-ffile-prefix-map=$PWD=." "-fmacro-prefix-map=$PWD=." "-ffile-prefix-map=$NDK=/ndk" -fdebug-compilation-dir=.)
fi

OUT="$PWD/build/scotland2/android-webutils"
mkdir -p "$OUT"
python3 - <<'PY'
from pathlib import Path
import hashlib,json
root=Path('port-deps/sources');d=json.loads((root/'libcurl-8.5.0-lock.json').read_text())
assert hashlib.sha256((root/'libcurl-8.5.0.a').read_bytes()).hexdigest()==d['binarySha256']
assert hashlib.sha256((root/d['headerArchive']).read_bytes()).hexdigest()==d['headerArchiveSha256']
link=Path('build/scotland2/mod-sdk/libcurl');target=(root/'libcurl-8.5.0').resolve()
if not link.exists():link.symlink_to(target,target_is_directory=True)
assert link.resolve()==target
PY
"$CC" "${REPRO_FLAGS[@]}" -std=c++20 -O1 -gline-tables-only -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -shared -fdeclspec \
 -DWEBUTILS_NO_JSON -DWEBUTILS_NO_BSML -DFMT_HEADER_ONLY -DMOD_ID='"WebUtils"' -DVERSION='"0.6.8"' \
 -Iports/scotland2/web-utils/upstream/shared -Iports/scotland2/web-utils/upstream/include \
 -Ibuild/scotland2/mod-sdk -Iport-deps/sources/fmt/include \
 ports/scotland2/web-utils/upstream/src/main.cpp ports/scotland2/web-utils/upstream/src/DownloaderUtility.cpp \
 ports/scotland2/web-utils/upstream/src/RatelimitedDispatcher.cpp \
 port-deps/sources/libcurl-8.5.0.a -llog -lz -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL \
 -Wl,-soname,libweb-utils.so -o "$OUT/libweb-utils.so"
python3 - "$OUT" <<'PY'
from pathlib import Path
import hashlib,json,sys
sys.path.insert(0,'tools');from audit_elf import Elf
out=Path(sys.argv[1]);p=out/'libweb-utils.so';elf=Elf(str(p))
original=Elf('port-results/coremods/elf/web-utils/0.6.8/libweb-utils.so')
missing=sorted(n for n in original.exports-elf.exports if 'WebUtils' in n)
assert not missing,missing
record=json.loads(Path('ports/scotland2/web-utils/provenance.json').read_text());record.update(originalPublicWebUtilsExportsPreserved=True,outputSha256=hashlib.sha256(p.read_bytes()).hexdigest(),curl=json.loads(Path('port-deps/sources/libcurl-8.5.0-lock.json').read_text()),patchSha256=hashlib.sha256(Path('ports/scotland2/web-utils/upstream.patch').read_bytes()).hexdigest(),scope='Original WebUtils 0.6.8 and original curl 8.5.0 archive. HTTPS verification enforced and explicit CA file supported via platform environment. Runtime requests not established by compilation.')
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
PY
