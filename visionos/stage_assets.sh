#!/bin/bash
# Stage the guest's data into the app's Documents container.
#
# Assets go here rather than into the .app for one reason: there are 2.2 GB of
# them and they change only when the APK does, while the code changes every few
# minutes. Bundling them would put a multi-minute upload in front of every
# build, which is the difference between the edit/run loop working on device and not.
# Code is the other way round — the guest libraries ride in the bundle, because
# P3 established AMFI's tolerance for a klepton-ld dylib *inside a bundle we
# signed* and established nothing about one pushed into Documents.
#
#   ./stage_assets.sh                 # the booted simulator
#   ./stage_assets.sh <device-udid>   # a physical Vision Pro
#
# Idempotent, but not incremental: it replaces what is there. The APK is
# load-bearing on its own, not just the unpacked tree — getPackageCodePath()
# hands the guest the APK path and it opens it as a zip.
#
# Which guest, and therefore which apk and which asset tree, comes from
# KLEPTON_TARGET (visionos/targets.py). The container layout mirrors the repo's:
# <container>/Documents/<tree>/assets and <container>/Documents/<tree>.apk, which
# is what kl_app_configure builds its paths from.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=".."
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
APK="$ROOT/$KLT_APK"
ASSETS="$ROOT/$KLT_ASSETS"
TREE="$KLT_TREE"

[ -d "$ASSETS" ] || { echo "!! $ASSETS missing"; exit 1; }
[ -f "$APK" ]    || { echo "!! $APK missing"; exit 1; }

# The OBB, for a SPLIT APPLICATION BINARY guest. Beat Saber 1.40 is one: the APK
# is 53 MB of code and the game's data ships beside it in
# main.<versionCode>.<package>.obb, which the guest finds through getObbDirs()
# -> <files>/obb (kl_jni.c, and the version code names the file, so a stale one
# reads as missing game data). 1.28 has no OBB and neither does Steam Link, so
# this is present-or-absent rather than required.
#
# It does NOT live in the repo — it is guest userdata, so it comes from the same
# directory a host run reads it from, and lands in <container>/android-files/obb,
# which is what kl_app.c hands kl_jni_set_files_dir. KL_OBB_DIR points elsewhere;
# KL_SKIP_OBB=1 leaves it alone, which is what you want on the second upload of
# a 1.3 GB file that has not changed.
#
# **The layout is the TARGET's, not a constant.** `KLT_OBB` is the path relative
# to the external-storage root that this guest looks in, and it is not one
# answer: a Unity guest asks Java (Context.getObbDirs() -> <files>/obb) and an
# Unreal guest builds Android's own <external>/Android/obb/<package>/ itself.
# Staging RE4 the Unity way put 8.5 GB somewhere libUE4 never looks, and both
# engines report a missing OBB by carrying on — so it is silent at both ends.
# The same relative path is used for the SOURCE under userdata, so a host run
# and a device run read the file from the same place.
OBB_REL="${KLT_OBB:-obb}"
OBB="${KL_OBB_DIR:-$HOME/Library/Application Support/Klepton/userdata/$KLT_NAME/$OBB_REL}"
if [ "${KL_SKIP_OBB:-0}" = 1 ] || ! compgen -G "$OBB/*.obb" > /dev/null 2>&1; then
  # Say WHICH directory was empty. "no OBB" is correct for four of the seven
  # targets and a staging mistake for the other three, and they looked the same.
  [ "${KL_SKIP_OBB:-0}" = 1 ] || echo "[stage] no *.obb in $OBB — staging none"
  OBB=""
fi

# The two files the unpacker left BESIDE assets/, which the guest reads through
# `<assets>/../` and which are not part of the asset tree, so nothing staged
# them. Both are small, both are read, and each one's absence is silent:
#
#   apktool.yml          carries versionCode/versionName. Missing, klj_guest_version
#                        falls back to 1.28's 545 — and a SPLIT APPLICATION BINARY
#                        guest builds its OBB NAME out of that number, so the
#                        1.3 GB of game data staged above is looked for under
#                        main.545... and never opened. That is what left 1.40 on
#                        device with `Unable to start Oculus XR Plugin` (the XR
#                        subsystem descriptors live in the OBB, at
#                        bin/Data/UnitySubsystems/OculusXRPlugin/) and Addressables
#                        reporting `No Location found for Key=AppInit` — a black
#                        immersive space, three layers from the missing file.
#   AndroidManifest.xml  carries the <meta-data> Bundle and the <uses-permission>
#                        list. Missing, every Oculus manifest setting reads as
#                        "not declared" and every permission check as DENIED.
#
# Present-or-absent, like the OBB: a tree unpacked some other way still stages.
META=()
for m in apktool.yml AndroidManifest.xml; do
  [ -f "$ROOT/$TREE/$m" ] && META+=("$ROOT/$TREE/$m")
done

# ...and they are stageable ON THEIR OWN, which is not a convenience.
#
#   ./stage_assets.sh --meta [<device-udid>]
#
# The gate in run.sh exists for a 2.2 GB upload and is skipped by default —
# `KL_SKIP_STAGE=1` is in the device loop as a matter of course. These two files
# are 12 KB, they change with the TREE rather than with the APK's size and
# mtime, and one of them names the OBB. Staged together with the assets they
# would have arrived only on a full re-upload: the run that found this had the
# right OBB on the device, the fix in the build, and still looked for
# main.545 — because nothing had re-staged since the fix. So run.sh calls this
# every run, and the stamp only ever gates the expensive half.
META_ONLY=0
if [ "${1:-}" = "--meta" ]; then META_ONLY=1; shift; fi

TARGET="${1:-}"

if [ -z "$TARGET" ]; then
  UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
  [ -n "$UDID" ] || { echo "!! no booted simulator"; exit 1; }
  CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data 2>/dev/null) || {
    echo "!! $BUNDLE_ID is not installed on $UDID — install the app first"; exit 1; }
  DEST="$CONTAINER/Documents"
  mkdir -p "$DEST/$TREE"
  if [ "$META_ONLY" = 1 ]; then
    [ ${#META[@]} -gt 0 ] && cp "${META[@]}" "$DEST/$TREE/"
    echo "[stage] simulator $UDID: ${#META[@]} metadata file(s) -> $DEST/$TREE"
    exit 0
  fi
  echo "[stage] simulator $UDID -> $DEST ($KLT_NAME)"
  rm -rf "$DEST/$TREE/assets"
  # A copy, not a symlink: the guest resolves paths by concatenation
  # and a link would work here but hide a real failure on device.
  cp -R "$ASSETS" "$DEST/$TREE/assets"
  cp "$APK" "$DEST/$KLT_APK"
  [ ${#META[@]} -gt 0 ] && cp "${META[@]}" "$DEST/$TREE/"
  if [ -n "$OBB" ]; then
    rm -rf "$DEST/android-files/$OBB_REL"
    mkdir -p "$DEST/android-files/$(dirname "$OBB_REL")"
    cp -RL "$OBB" "$DEST/android-files/$OBB_REL"
  fi
  if [ -n "$KLT_QTPLUGINS" ]; then
    rm -rf "$DEST/$TREE/qtplugins"
    mkdir -p "$DEST/$TREE/qtplugins"
    cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$DEST/$TREE/qtplugins/"
  fi
  echo "[stage] done: $(du -sh "$DEST" | cut -f1)"
  exit 0
fi

# Beat Saber's 2.2 GB is the reason any of this exists; Steam Link's is 44 MB
# and takes seconds. Saying which is which up front is the difference between
# waiting and wondering.
copy() {   # <source> <destination-relative-to-container>
  xcrun devicectl device copy to --device "$TARGET" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
    --source "$1" --destination "$2"
}
copy_meta() {
  for m in "${META[@]:-}"; do
    [ -n "$m" ] && copy "$m" "Documents/$TREE/$(basename "$m")" > /dev/null
  done
}
if [ "$META_ONLY" = 1 ]; then
  copy_meta
  echo "[stage] device $TARGET: ${#META[@]} metadata file(s) -> Documents/$TREE"
  exit 0
fi
# Beat Saber's 2.2 GB is the reason any of this exists; Steam Link's is 44 MB
# and takes seconds. Saying which is which up front is the difference between
# waiting and wondering.
echo "[stage] device $TARGET, $KLT_NAME ($(du -shc "$ASSETS" "$APK" ${OBB:+"$OBB"} | tail -1 | cut -f1))"
copy "$ASSETS" "Documents/$TREE/assets"
copy "$APK"    "Documents/$KLT_APK"
copy_meta
# The OBB, FILE BY FILE and through the RESOLVED path — not `copy "$OBB"`.
#
# The obb directory a host run reads is deliberately symlinks into bonelab_obb/
# (6.8 GB is not worth a second copy on this machine), and `devicectl device
# copy to` copies a symlink AS a symlink. The device then holds two ~175-byte
# files with exactly the right names, pointing at a path that does not exist
# there — so every name-based check passes, `getObbDirs()` finds them, and the
# only symptom is Unity saying the OBB's GUID is `''` a thousand log lines
# later. kl_jni.c's obb census now stats them and says so by name; this is the
# other half, which is not staging a link in the first place.
#
# Passing each resolved path dereferences naturally, and it also means the
# destination is the FILE's path rather than the directory's parent, which is
# why this cannot just gain a flag.
if [ -n "$OBB" ]; then
  for f in "$OBB"/*.obb; do
    [ -e "$f" ] || continue          # a dangling link on THIS machine too
    echo "[stage] device $TARGET, $KLT_NAME $f"
    real=$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$f")
    copy "$real" "Documents/android-files/$OBB_REL/$(basename "$f")"
    echo "[stage] done $f"
  done
fi
# The Qt plugin .so files, when the target asks for them. They are DATA, not a
# loader path — see the `qtplugins` note in targets.py: Qt globs this directory
# and parses each candidate's ELF metadata before it will dlopen it, and the
# dlopen itself still resolves to the signed framework in the bundle.
# devicectl copies a directory, so they are staged through one locally first.
if [ -n "$KLT_QTPLUGINS" ]; then
  STAGE=$(mktemp -d)/qtplugins
  mkdir -p "$STAGE"
  cp "$ROOT/$KLT_QTPLUGINS"/libplugins_*.so "$STAGE/"
  copy "$STAGE" "Documents/$TREE/qtplugins"
  rm -rf "$(dirname "$STAGE")"
fi
echo "[stage] done"
