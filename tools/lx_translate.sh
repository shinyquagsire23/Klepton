#!/bin/bash
# Translate Linux/glibc ARM64 ELFs for the Linux process runtime (runtime/linux/).
#
#   tools/lx_translate.sh <out-dir> <elf-or-directory>...
#
# Every real ELF file given, or found under a directory given, becomes one ad-hoc
# signed dylib in <out-dir>. Symlinks are skipped: the runtime resolves a guest
# path to the file it finally names before it looks for a translation.
#
# The output name comes from `klepton-ld --print-name`: the file name plus a
# fingerprint of the content (runtime/linux/kl_lx_name.h says why it has to be
# both).
#
# KLEPTON_LD, PLATFORM (default macos) and JOBS can be overridden.
set -euo pipefail
OUT="$1"; shift
LD="${KLEPTON_LD:-$(dirname "$0")/../build/klepton-ld}"
PLATFORM="${PLATFORM:-macos}"
mkdir -p "$OUT"

one() {
    f="$1"
    [ "$(head -c 4 "$f" | xxd -p)" = "7f454c46" ] || exit 0
    name="$("$LD" --print-name "$f")"; name="${name%.dylib}"
    [ -e "$OUT/$name.dylib" ] && [ "$OUT/$name.dylib" -nt "$f" ] && [ "$OUT/$name.dylib" -nt "$LD" ] && exit 0
    if "$LD" --abi linux --quiet --platform "$PLATFORM" --install-name "@rpath/$name.dylib" \
             "$f" -o "$OUT/$name.dylib" 2> "$OUT/$name.log"; then
        codesign -s - -f "$OUT/$name.dylib" 2>/dev/null
        [ -s "$OUT/$name.log" ] || rm -f "$OUT/$name.log"
    else
        echo "FAILED $f: $(tail -1 "$OUT/$name.log")"
    fi
}
export -f one
export LD OUT PLATFORM

for src in "$@"; do
    if [ -d "$src" ]; then find "$src" -type f -size +0; else [ -L "$src" ] || echo "$src"; fi
done | xargs -P "${JOBS:-8}" -I{} bash -c 'one "$@"' _ {}
