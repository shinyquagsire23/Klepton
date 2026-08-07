#!/bin/bash
# Build, install, stage and run the Klepton app — simulator or device.
#
#   ./run.sh              # the booted visionOS simulator (rung 2, dyld)
#   ./run.sh device       # a physical Vision Pro   (rung 3, AMFI)
#
# Env overrides:
#   KLEPTON_DEVICE=<udid>   skip device auto-detection
#   KLEPTON_TEAM=<teamid>   override signing team
#   KLEPTON_BUNDLE_ID=...   override bundle id
#   KL_SKIP_STAGE=1         do not re-upload assets (device: they persist)
set -euo pipefail
cd "$(dirname "$0")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-dev.klepton.app}"
MODE="${1:-sim}"

echo "[1/5] runtime + guest translations…"
(cd .. && make -s xros)
./mkguest.sh | tail -1

echo "[2/5] project…"
python3 gen_xcodeproj.py | head -1

if [ "$MODE" = "device" ]; then
  # Device discovery goes through JSON: device names contain non-breaking
  # spaces, so column parsing silently picks up the wrong field.
  find_device() {
    local json; json=$(mktemp)
    xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
    python3 -c '
import json, sys
try:
    devs = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    sys.exit(0)
phys = [d for d in devs
        if d.get("hardwareProperties", {}).get("reality") == "physical"
        and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
if not phys:
    sys.exit(0)
phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
print(phys[0]["identifier"])
' "$json"
    rm -f "$json"
  }
  DEVID="${KLEPTON_DEVICE:-$(find_device || true)}"
  [ -n "$DEVID" ] || { echo "!! no physical Vision Pro — pair it in Xcode, or set KLEPTON_DEVICE"; exit 1; }

  echo "[3/5] building for device ${DEVID}…"
  # generic/platform, not id= : the build must not depend on the device being awake
  set +e
  xcodebuild -project Klepton.xcodeproj -scheme Klepton -configuration Debug \
    -destination 'generic/platform=visionOS' -derivedDataPath build/dd-device \
    -allowProvisioningUpdates build 2>&1 | grep -E 'error:|Signing Identity|\*\* BUILD'
  set -e
  APP="build/dd-device/Build/Products/Debug-xros/Klepton.app"
  [ -d "$APP" ] || { echo "!! no .app at $APP"; exit 1; }

  echo "[4/5] installing…"
  xcrun devicectl device install app --device "$DEVID" "$APP" | tail -2
  # Installing can rotate the data container, so assets are staged *after*.
  [ -n "${KL_SKIP_STAGE:-}" ] || ./stage_assets.sh "$DEVID"

  echo "[5/5] launching (report also renders on-screen)…"
  xcrun devicectl device process launch --device "$DEVID" --console \
    --environment-variables '{"KL_AUTOBOOT":"1"}' "$BUNDLE_ID"
  exit 0
fi

UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
[ -n "$UDID" ] || { echo "!! no booted visionOS simulator"; exit 1; }

echo "[3/5] building for the simulator…"
set +e
xcodebuild -project Klepton.xcodeproj -scheme Klepton \
  -destination "platform=visionOS Simulator,id=$UDID" \
  -derivedDataPath build/dd-sim CODE_SIGNING_ALLOWED=NO build 2>&1 \
  | grep -E 'error:|\*\* BUILD'
set -e
APP="build/dd-sim/Build/Products/Debug-xrsimulator/Klepton.app"
[ -d "$APP" ] || { echo "!! no .app at $APP"; exit 1; }

echo "[4/5] installing + staging…"
xcrun simctl install "$UDID" "$APP"
# After install, not before: simctl rotates the data container on reinstall, so
# assets staged earlier end up in an orphaned one and the app reports them
# missing. Cost a confusing run once.
[ -n "${KL_SKIP_STAGE:-}" ] || ./stage_assets.sh | tail -1

echo "[5/5] launching…"
SIMCTL_CHILD_KL_AUTOBOOT=1 xcrun simctl launch --terminate-running-process "$UDID" "$BUNDLE_ID"
LOG="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)/Documents/klepton-boot.log"
echo "    log: $LOG"
n=0; until [ -f "$LOG" ] || [ $n -ge 20 ]; do sleep 1; n=$((n+1)); done
sleep 10
LC_ALL=C grep -aE 'EXIT CRITERION|FAIL|translated dylib|natives registered|ids requested' "$LOG" || true
