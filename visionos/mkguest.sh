#!/bin/bash
# Translate this target's guest libraries and wrap each as an XCFramework.
#
# Why frameworks rather than a directory of .dylib files:
# Xcode code-signs what it embeds under Frameworks/, and a loose Mach-O
# anywhere else in the bundle is only *sealed* by the outer signature, which is
# not the same thing and is not what AMFI accepted in P3/P12. kl_load_auto
# knows both layouts, so `make dylibs` can keep writing bare .dylib files for
# the host.
#
# Both slices are built BY DEFAULT because they are genuinely different images:
# the platform goes into LC_BUILD_VERSION, and a visionos-stamped dylib is
# refused by the simulator's dyld and vice versa. `XROS_PLATFORMS=xros` (or
# `=xrsim`) translates only one, which is what a run of the app needs and half
# the work — run.sh passes the one its mode will install. The name matches the
# make variable that selects the runtime's slices, because they must agree: an
# app whose runtime is device-only and whose guest is simulator-only fails as an
# arch mismatch naming neither.
#
# Which guest comes from KLEPTON_TARGET (visionos/targets.py). The output goes
# into a per-target subdirectory of Frameworks/ so that building the second app
# does not overwrite the first one's embedded guest in place — a bundle ID
# separates the installed apps and nothing at all about their build outputs.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
LD="$ROOT/build/klepton-ld"
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
SRC="$ROOT/$KLT_SRCDIR"
OUT="Frameworks/$KLT_NAME"
LIBS="$KLT_LIBS"

# Which slices — see the header. The names are the make variable's (xros,
# xrsim); klepton-ld and the framework directories speak xrsimulator, so the
# mapping is here and nowhere else.
PLATS=""
for P in ${XROS_PLATFORMS:-xros xrsim}; do
  case "$P" in
    xros)  PLATS="$PLATS xros" ;;
    xrsim) PLATS="$PLATS xrsimulator" ;;
    *)     echo "!! XROS_PLATFORMS: '$P' is not one of xros xrsim"; exit 1 ;;
  esac
done
FIRST_PLAT=$(printf '%s' "$PLATS" | awk '{print $1}')

[ -x "$LD" ] || { echo "!! $LD missing — run 'make build/klepton-ld' first"; exit 1; }

# The target's own subdirectory is cleared, not the whole of Frameworks/ — that
# is shared with mkangle.sh, and wiping it here made the two scripts
# order-dependent in a way nothing would have reported except a missing ANGLE at
# runtime.
rm -rf "build/guest/$KLT_NAME" "$OUT"; mkdir -p "build/guest/$KLT_NAME" "$OUT"

for NAME in $LIBS; do
  [ -f "$SRC/$NAME.so" ] || { echo "!! $SRC/$NAME.so missing"; exit 1; }
  # Bundle identifiers admit only alphanumerics, '-' and '.'. Several of these
  # names contain '_', and libc++_shared contains '+' — which the previous
  # underscores-only rewrite let through, and Xcode rejects at the EMBED step
  # rather than at build: "had an invalid CFBundleIdentifier in its Info.plist".
  ID_NAME=$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9.-' '-')
  for PLAT in $PLATS; do
    case "$PLAT" in
      xros)        KLPLAT=visionos    ;;
      xrsimulator) KLPLAT=visionossim ;;
    esac
    FW="build/guest/$KLT_NAME/$PLAT/$NAME.framework"
    mkdir -p "$FW"
    "$LD" "$SRC/$NAME.so" -o "$FW/$NAME" --platform "$KLPLAT" \
          --install-name "@rpath/$NAME.framework/$NAME" --quiet
    cat > "$FW/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>dev.klepton.guest.$ID_NAME</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>2.0</string>
</dict></plist>
EOF
  done
  rm -rf "$OUT/$NAME.xcframework"
  FWARGS=()
  for PLAT in $PLATS; do
    FWARGS+=(-framework "build/guest/$KLT_NAME/$PLAT/$NAME.framework")
  done
  xcodebuild -create-xcframework "${FWARGS[@]}" \
      -output "$OUT/$NAME.xcframework" > /dev/null
  printf '  %-22s %s bytes\n' "$NAME.xcframework" \
      "$(stat -f%z "build/guest/$KLT_NAME/$FIRST_PLAT/$NAME.framework/$NAME")"
done

echo "[mkguest] $KLT_NAME done -> $OUT/"
