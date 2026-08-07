#!/bin/bash
# Builds the two probe frameworks as XCFrameworks (device + simulator slices).
#   KleptonProbeA — stock linker defaults
#   KleptonProbeB — 64KB segment alignment, matching every translated guest .so
set -euo pipefail
cd "$(dirname "$0")"
OUT="Frameworks"
rm -rf build "$OUT"; mkdir -p build "$OUT"

build_slice() {          # <name> <platform> <extra ld flags...>
  local NAME="$1" PLAT="$2"; shift 2
  local TARGET SDK DIR FW
  case "$PLAT" in
    xros)        TARGET="arm64-apple-xros1.0" ;;
    xrsimulator) TARGET="arm64-apple-xros1.0-simulator" ;;
  esac
  SDK="$(xcrun --sdk "$PLAT" --show-sdk-path)"
  DIR="build/$PLAT"; FW="$DIR/$NAME.framework"
  mkdir -p "$FW"
  printf 'int klepton_probe_value(void) { return 0x4B4C; }\n' > "build/$NAME.c"
  clang -target "$TARGET" -isysroot "$SDK" -dynamiclib -O1 \
        -install_name "@rpath/$NAME.framework/$NAME" \
        "$@" -o "$FW/$NAME" "build/$NAME.c"
  cat > "$FW/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>dev.klepton.probe.$NAME</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>1.0</string>
</dict></plist>
EOF
}

make_xcframework() {     # <name> <extra ld flags...>
  local NAME="$1"; shift
  build_slice "$NAME" xros         "$@"
  build_slice "$NAME" xrsimulator  "$@"
  xcodebuild -create-xcframework \
      -framework "build/xros/$NAME.framework" \
      -framework "build/xrsimulator/$NAME.framework" \
      -output "$OUT/$NAME.xcframework"
  local SZ
  # NB: no `exit` in awk -- it SIGPIPEs otool, which trips pipefail+set -e
  SZ=$(otool -l "build/xros/$NAME.framework/$NAME" | awk '/segname __TEXT/{f=1} f&&/vmsize/&&!s{print $2;s=1}')
  echo "  $NAME.xcframework   __TEXT vmsize=$SZ"
}

# KleptonGuest — the P3 subject. NOT built by clang: this framework's binary is
# emitted by klepton-ld from a real guest .so, so it is the *hand-emitted* Mach-O
# that S0.2 never tested. S0.2 proved a signed dylib loads; the open question is
# whether one dyld's own linker never touched survives AMFI on device.
make_guest_framework() {
  local NAME="KleptonGuest"
  local LD="../../build/klepton-ld"
  local SO="../../beatsaber/lib/arm64-v8a/libunityopus.so"
  [ -x "$LD" ] || { echo "  !! $LD missing — run 'make build/klepton-ld' first"; return 1; }
  [ -f "$SO" ] || { echo "  !! $SO missing"; return 1; }

  local PLAT KLPLAT DIR FW
  for PLAT in xros xrsimulator; do
    case "$PLAT" in
      xros)        KLPLAT=visionos    ;;
      xrsimulator) KLPLAT=visionossim ;;
    esac
    DIR="build/$PLAT"; FW="$DIR/$NAME.framework"
    mkdir -p "$FW"
    "$LD" "$SO" -o "$FW/$NAME" --platform "$KLPLAT" \
         --install-name "@rpath/$NAME.framework/$NAME" --quiet
    cat > "$FW/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>dev.klepton.probe.$NAME</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>1.0</string>
</dict></plist>
EOF
  done
  xcodebuild -create-xcframework \
      -framework "build/xros/$NAME.framework" \
      -framework "build/xrsimulator/$NAME.framework" \
      -output "$OUT/$NAME.xcframework"
  echo "  $NAME.xcframework   klepton-ld output ($(stat -f%z build/xros/$NAME/../$NAME.framework/$NAME 2>/dev/null || stat -f%z build/xros/$NAME.framework/$NAME) bytes)"
}

echo "[mkframeworks] building probe XCFrameworks…"
make_xcframework KleptonProbeA
make_xcframework KleptonProbeB -Wl,-segalign,0x10000
make_guest_framework
echo "[mkframeworks] done -> $OUT/"
