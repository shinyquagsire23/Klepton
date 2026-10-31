#!/bin/bash
# Compile the actual Scotland2 loader core for macOS and visionOS. No install.
set -euo pipefail
cd "$(dirname "$0")/../.."
SDK_NAME="${1:-macosx}"
case "$SDK_NAME" in
  macosx) TARGET=arm64-apple-macos14.0 ;;
  xros) TARGET=arm64-apple-xros26.0 ;;
  *) echo 'Use macosx or xros.' >&2; exit 2 ;;
esac
REPRO_FLAGS=(-fdebug-compilation-dir=.)
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then
  REPRO_FLAGS=("-ffile-prefix-map=$PWD=." "-fmacro-prefix-map=$PWD=." -fdebug-compilation-dir=.)
fi
DIAGNOSTICS=1
if [ "${SC2_BUILD_MODE:-diagnostic}" = release ]; then DIAGNOSTICS=0; fi
SDK=$(xcrun --sdk "$SDK_NAME" --show-sdk-path)
OUT="build/scotland2/$SDK_NAME"
mkdir -p "$OUT"
for SRC in ports/scotland2/upstream/src/loader.cpp \
           ports/scotland2/upstream/src/modloader.cpp \
           ports/scotland2/src/apple-platform.cpp ports/scotland2/src/apple-elf.cpp; do
  xcrun clang++ "${REPRO_FLAGS[@]}" -target "$TARGET" -isysroot "$SDK" -std=c++20 -O1 -g \
    -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function \
    -DSL2_KLEPTON -DSC2_DIAGNOSTICS="$DIAGNOSTICS" -DNO_STAT_DUMPS -DFMT_HEADER_ONLY -DFMT_USE_CONSTEVAL=0 \
    -DMOD_VERSION='"0.1.7-klepton-core"' \
    -Iports/scotland2/include -Iports/scotland2/upstream/include \
    -Iports/scotland2/upstream/shared -Iport-deps/sources/fmt/include \
    -c "$SRC" -o "$OUT/$(basename "$SRC" .cpp).o"
done
xcrun ar rcs "$OUT/libscotland2-core.a" "$OUT/loader.o" "$OUT/modloader.o" "$OUT/apple-platform.o" "$OUT/apple-elf.o"
echo "Scotland2 loader core: $OUT/libscotland2-core.a ($TARGET)"
