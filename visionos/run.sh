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
  # xcodebuild's status, through the pipe. Testing for the .app is NOT enough: a
  # failed *incremental* build leaves the previous, complete and perfectly valid
  # bundle in place, so the install and launch below both succeed and run the OLD
  # binary — and the log then reads as "the change had no effect", which is the
  # most expensive possible way to be told about a compile error.
  BUILD_RC=${PIPESTATUS[0]}
  set -e
  APP="build/dd-device/Build/Products/Debug-xros/Klepton.app"
  [ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
  [ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }

  echo "[4/5] installing…"
  xcrun devicectl device install app --device "$DEVID" "$APP" | tail -2
  # Installing can rotate the data container, so assets are staged *after*.
  [ -n "${KL_SKIP_STAGE:-}" ] || ./stage_assets.sh "$DEVID"

  echo "[5/5] launching (report also renders on-screen)…"
  # KL_FRAMES carries the app past P4's initJni gate into the Android lifecycle
  # (P5.4) — which is where libil2cpp and its 3,083 veneers first load under
  # AMFI. Unset, the run stops at initJni, which is P4's measurement and has to
  # stay takeable.
  ENVJSON='{"KL_AUTOBOOT":"1"'
  [ -z "${KL_FRAMES:-}" ] || ENVJSON="$ENVJSON,\"KL_FRAMES\":\"$KL_FRAMES\""
  [ -z "${KL_PERMISSIVE:-}" ] || ENVJSON="$ENVJSON,\"KL_PERMISSIVE\":\"$KL_PERMISSIVE\""
  ENVJSON="$ENVJSON}"
  if [ -z "${KL_FRAMES:-}" ]; then
    # P4's shape: --console blocks until the app terminates, which for a UI app
    # means until it is quit by hand. Fine for a run that finishes in seconds.
    xcrun devicectl device process launch --device "$DEVID" --console \
      --environment-variables "$ENVJSON" "$BUNDLE_ID"
  else
    # A lifecycle run takes minutes and the app still never exits on its own, so
    # --console would just sit there waiting to be force-quit — and force-quitting
    # is indistinguishable from the run having finished. Launch detached and watch
    # the log instead: it is line-buffered precisely so it can be read live.
    xcrun devicectl device process launch --device "$DEVID" \
      --environment-variables "$ENVJSON" "$BUNDLE_ID" | tail -2
    echo
    echo "    watching the log; it is done when it stops growing (Ctrl-C to stop)"
    LOCAL="${KL_LOG_OUT:-/tmp/klepton-device.log}"
    last=-1; same=0
    for _ in $(seq 1 "${KL_WATCH:-120}"); do
      sleep 5
      xcrun devicectl device copy from --device "$DEVID" \
        --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
        --source Documents/klepton-boot.log --destination "$LOCAL" >/dev/null 2>&1 || continue
      n=$(wc -l < "$LOCAL" | tr -d ' ')
      printf '    %s lines\n' "$n"
      if [ "$n" = "$last" ]; then
        same=$((same+1))
        # Quiet has to outlast the in-app watchdog (KL_ALARM, default 120 s) or
        # the poller declares the run over before the thing that would explain it
        # has had a chance to fire — which is how the first stalled device run was
        # read as "no report" when the report simply had not happened yet.
        [ "$same" -lt "${KL_QUIET:-32}" ] || { echo "    log stopped growing"; break; }
      else
        same=0
      fi
      last="$n"
    done
    echo "    log: $LOCAL"
  fi
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
BUILD_RC=${PIPESTATUS[0]}
set -e
APP="build/dd-sim/Build/Products/Debug-xrsimulator/Klepton.app"
[ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
[ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }

echo "[4/5] installing + staging…"
xcrun simctl install "$UDID" "$APP"
# After install, not before: simctl rotates the data container on reinstall, so
# assets staged earlier end up in an orphaned one and the app reports them
# missing. Cost a confusing run once.
[ -n "${KL_SKIP_STAGE:-}" ] || ./stage_assets.sh | tail -1

echo "[5/5] launching…"
# `env`, not a bare prefix: bash only treats NAME=value as an assignment when it
# is literally there before expansion, so ${KL_FRAMES:+SIMCTL_CHILD_KL_FRAMES=…}
# is parsed as a *command* name and dies with "command not found".
env SIMCTL_CHILD_KL_AUTOBOOT=1 \
  ${KL_FRAMES:+SIMCTL_CHILD_KL_FRAMES="$KL_FRAMES"} \
  ${KL_ALARM:+SIMCTL_CHILD_KL_ALARM="$KL_ALARM"} \
  ${KL_PERMISSIVE:+SIMCTL_CHILD_KL_PERMISSIVE="$KL_PERMISSIVE"} \
  xcrun simctl launch --terminate-running-process "$UDID" "$BUNDLE_ID"
LOG="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)/Documents/klepton-boot.log"
echo "    log: $LOG"
n=0; until [ -f "$LOG" ] || [ $n -ge 20 ]; do sleep 1; n=$((n+1)); done
sleep 10
LC_ALL=C grep -aE 'EXIT CRITERION|FAIL|translated dylib|natives registered|ids requested' "$LOG" || true
