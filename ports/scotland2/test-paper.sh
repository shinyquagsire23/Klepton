#!/bin/bash
# Actual released Paper binary; isolated Mac test, never installs an app.
set -euo pipefail
cd "$(dirname "$0")/../.."
SC2_TEST_SET=paper bash ports/scotland2/package.sh macosx
BUNDLE="$PWD/build/scotland2/macosx-paper/bundle"
META="$BUNDLE/Scotland2Metadata"
FRAMEWORKS="$BUNDLE/Frameworks"
make build/t_scotland2_paper
USERDATA=$(mktemp -d "$PWD/build/scotland2/paper-userdata-XXXXXXXX")
KL_SCOTLAND2=1 KL_DYLIB_DIR="$FRAMEWORKS" KL_SCOTLAND2_ROOT="$META" \
  ./build/t_scotland2_paper "$USERDATA"
