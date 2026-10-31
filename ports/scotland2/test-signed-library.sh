#!/bin/bash
# Run a real CoreMods dependency through the actual Scotland2 core on this Mac.
# Creates no headset app, provisioning profile, or new bundle identifier.
set -euo pipefail
cd "$(dirname "$0")/../.."
make build/klepton-ld build/t_scotland2
TEST_DIR=$(mktemp -d "$PWD/build/scotland2/signed-test-XXXXXXXX")
mkdir -p "$TEST_DIR/metadata/"{libs,early_mods,mods,shims} "$TEST_DIR/translated" "$TEST_DIR/userdata"
LIBRARY=$(python3 - <<'PY'
import json
from pathlib import Path
root = Path('port-results/coremods')
lock = json.loads((root / 'coremods-lock.json').read_text())
matches = [b for b in lock['binaries'] if b['mod'] == 'tinyxml2' and b['file'].endswith('/libtinyxml2.so')]
assert len(matches) == 1
path = root / matches[0]['file']
import hashlib
assert hashlib.sha256(path.read_bytes()).hexdigest() == matches[0]['sha256']
print(path.resolve())
PY
)
cp "$LIBRARY" "$TEST_DIR/metadata/libs/libtinyxml2.so"
./build/klepton-ld "$LIBRARY" -o "$TEST_DIR/translated/libtinyxml2.dylib" --platform macos --quiet
codesign --force --sign - "$TEST_DIR/translated/libtinyxml2.dylib"
codesign --verify --strict "$TEST_DIR/translated/libtinyxml2.dylib"
KL_DYLIB_DIR="$TEST_DIR/translated" KL_FILES_DIR="$TEST_DIR/userdata" \
  ./build/t_scotland2 "$TEST_DIR/metadata"
echo "Signed-library test artifacts: $TEST_DIR"
