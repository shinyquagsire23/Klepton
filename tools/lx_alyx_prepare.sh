#!/bin/bash
# One-time preparation of the Alyx game tree for tools/lx_alyx.sh.
#
#   tools/lx_alyx_prepare.sh [game dir, default ../alyx/game]
#
# 1. The symlinks hlvr.sh would make at launch. Steam depots cannot carry links,
#    so the depot ships zero-byte placeholders (libpango-1.0.so and friends) and
#    the launch script replaces them; nothing runs that script here. The list is
#    read out of hlvr.sh itself, so it follows the depot.
# 2. Translations of every ELF in the two linuxsteamrtarm64 directories.
set -euo pipefail
cd "$(dirname "$0")/.."
GAME="${1:-../alyx/game}"
BIN="$GAME/bin/linuxsteamrtarm64"

sed -n '/linuxsteamrtarm64)/,/;;/p' "$GAME/hlvr.sh" | grep -o '"[^":]*:[^":]*"' | tr -d '"' |
while IFS=: read -r link target; do
    [ -e "$BIN/$target" ] || [ -L "$BIN/$target" ] || { echo "no target for $link -> $target"; continue; }
    if [ -L "$BIN/$link" ] && [ "$(readlink "$BIN/$link")" = "$target" ]; then continue; fi
    rm -f "$BIN/$link"
    ln -s "$target" "$BIN/$link"
    echo "linked $link -> $target"
done

tools/lx_translate.sh build/lx-alyx "$BIN" "$GAME/hlvr/bin/linuxsteamrtarm64"
echo "$(ls build/lx-alyx/*.dylib | wc -l | tr -d ' ') translations in build/lx-alyx"
