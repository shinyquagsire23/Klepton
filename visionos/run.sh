#!/bin/bash
# Build, install, stage and run the Klepton app — simulator or device.
#
#   ./run.sh              # the booted visionOS simulator (rung 2, dyld)
#   ./run.sh device       # a physical Vision Pro   (rung 3, AMFI)
#
# Env overrides:
#   KLEPTON_TARGET=<name>   which guest — `python3 targets.py --list`
#   KLEPTON_DEVICE=<udid>   skip device auto-detection
#   KLEPTON_TEAM=<teamid>   override signing team
#   KLEPTON_BUNDLE_ID=...   override bundle id
#   KL_SKIP_STAGE=1         never stage, even on a target that has never had it
#   KL_STAGE=1              always stage, whatever the stamp says
#
# Everything downstream of the target — the product name, and with it the
# .xcodeproj, the scheme, the .app and the derived-data directory — comes from
# targets.py, so two apps built from this tree never write to the same place.
set -euo pipefail
cd "$(dirname "$0")"
eval "$(python3 targets.py "${KLEPTON_TARGET:-}")"
export KLEPTON_TARGET="$KLT_NAME"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
PRODUCT="$KLT_PRODUCT"
MODE="${1:-sim}"

# --- Staging: skip by default, but not blindly ------------------------------
#
# The 2.2 GB upload is a ~20 minute step and the assets change only when the APK
# does, so re-running it every build is the difference between the device loop
# working and not. Hence: skip by default.
#
# The trap that stops this being a plain `KL_SKIP_STAGE=1` default is that a
# target which has NEVER been staged then launches with no assets, and Unity
# does not report that as missing assets — it reports it three layers up as
# "Not enough storage space to install required resources" (trap 6c). That is
# an expensive sentence to debug when the real answer is "nothing was uploaded".
#
# So a stamp records that this target has been staged with this APK. No stamp
# means stage; stamp means skip. The stamp is keyed on the APK's size and mtime,
# so replacing the APK re-stages on its own rather than needing to be
# remembered. It cannot know about a data container the OS rotated underneath
# us — KL_STAGE=1 is the answer to that, and the symptom is the same sentence.
STAMP_DIR="build/staged"
# ...and the stamp is keyed on the LAYOUT as well as the APK, because what gets
# staged is not fixed. SL-18 added the Qt plugin .so files, and a stamp that
# knew only about the APK would have skipped staging them on every machine that
# had ever staged this target — a shell that aborts with `Could not find the Qt
# platform plugin "virtual"` and a run.sh that says the assets are already
# there. Bump this whenever stage_assets.sh stages something new.
STAGE_SCHEMA=2
stage_stamp() {   # <target-key>
  local apk="../$KLT_APK" sig=""
  [ -f "$apk" ] && sig=$(stat -f '%z-%m' "$apk" 2>/dev/null || true)
  echo "$STAMP_DIR/$(echo "$KLT_NAME-$1-$BUNDLE_ID-$sig-v$STAGE_SCHEMA" \
       | tr -c 'A-Za-z0-9._-' '_')"
}
# stage_if_needed <target-key> <stage_assets.sh args...>
stage_if_needed() {
  local key="$1"; shift
  local stamp; stamp=$(stage_stamp "$key")
  if [ -n "${KL_SKIP_STAGE:-}" ]; then
    echo "  assets: skipped (KL_SKIP_STAGE=1)"
  elif [ -z "${KL_STAGE:-}" ] && [ -f "$stamp" ]; then
    echo "  assets: already staged for this target and APK (KL_STAGE=1 to redo)"
  else
    ./stage_assets.sh "$@"
    mkdir -p "$STAMP_DIR" && : > "$stamp"
  fi
}

# Knobs forwarded from this shell into the app, on both the device and simulator
# paths. A list rather than a line each, because the two paths spell the same
# thing differently (devicectl JSON vs SIMCTL_CHILD_*) and they were already
# drifting apart. KL_ANGLE_DIR is deliberately NOT here: only the app knows
# where its own bundle is, so kl_app_configure sets it (and an explicit one
# still wins, since it is set with overwrite=0).
#
# KL_IMMERSIVE was missing from this list until 2026-08-07, which made the
# documented `KL_IMMERSIVE=1 ./run.sh` a no-op: the shell had it, the app never
# did, and the run took P4's window-and-report path while looking like the
# compositor had failed to come up.
# Everything KL_* in this shell is forwarded, minus the few that belong to this
# script rather than to the app. A hand-maintained allow-list was the previous
# shape and it silently dropped KL_IMMERSIVE for a whole session: the shell had
# it, the app never did, and the run took P4's window path while looking like the
# compositor had failed to come up. A knob that is set and not delivered is worse
# than one that does not exist, so the default is now "forward it".
FORWARD_SKIP="KL_ANGLE_DIR KL_SKIP_STAGE KL_STAGE KL_LOG_OUT KL_WATCH KL_QUIET KL_KEEP_LONGEST"
FORWARD=""
for K in $(env | sed -n 's/^\(KL_[A-Za-z0-9_]*\)=.*/\1/p' | sort); do
  case " $FORWARD_SKIP " in *" $K "*) continue ;; esac
  FORWARD="$FORWARD $K"
done

echo "[1/5] runtime + guest translations ($KLT_NAME)…"
# klepton-ld as well as the runtime, and not for tidiness: the translator bakes
# KLX_TSD_SLOT into every veneer it emits, so a stale one produces dylibs the
# fresh runtime refuses — "translated against TSD slot 300 but this runtime uses
# 500". The loader catches it by name, which is the right failure, but it is not
# a failure anyone should have to have.
(cd .. && make -s build/klepton-ld xros)
# Keep the embedded ANGLE current, but only when the checkout is already
# there: this must not turn `run.sh` into a surprise 12 GB clone. Without it,
# mkangle.sh below stops and names the bootstrap command instead.
if [ -d ../vendor/.git ]; then (cd .. && make -s angle-xros angle-xrsim); fi
./mkguest.sh | tail -1
./mkangle.sh | tail -1

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
  xcodebuild -project "$PRODUCT.xcodeproj" -scheme "$PRODUCT" -configuration Debug \
    -destination 'generic/platform=visionOS' -derivedDataPath "build/dd-device-$KLT_NAME" \
    -allowProvisioningUpdates build 2>&1 | grep -E 'error:|Signing Identity|\*\* BUILD'
  # xcodebuild's status, through the pipe. Testing for the .app is NOT enough: a
  # failed *incremental* build leaves the previous, complete and perfectly valid
  # bundle in place, so the install and launch below both succeed and run the OLD
  # binary — and the log then reads as "the change had no effect", which is the
  # most expensive possible way to be told about a compile error.
  BUILD_RC=${PIPESTATUS[0]}
  set -e
  APP="build/dd-device-$KLT_NAME/Build/Products/Debug-xros/$PRODUCT.app"
  [ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
  [ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }

  echo "[4/5] installing…"
  xcrun devicectl device install app --device "$DEVID" "$APP" | tail -2
  # Installing can rotate the data container, so assets are staged *after*.
  stage_if_needed "$DEVID" "$DEVID"

  echo "[5/5] launching (report also renders on-screen)…"
  # KL_FRAMES carries the app past P4's initJni gate into the Android lifecycle
  # (P5.4) — which is where libil2cpp and its 3,083 veneers first load under
  # AMFI. Unset, the run stops at initJni, which is P4's measurement and has to
  # stay takeable.
  ENVJSON='{"KL_AUTOBOOT":"1"'
  for K in $FORWARD; do
    eval "V=\${$K:-}"
    [ -z "$V" ] || ENVJSON="$ENVJSON,\"$K\":\"$V\""
  done
  ENVJSON="$ENVJSON}"
  # Which launch mode: --console only suits a run that ENDS. Two things make a
  # run open-ended, and it used to be only one of them — KL_FRAMES. The app now
  # opens the immersive space by default, and an immersive run has no frame
  # budget to exhaust, so `./run.sh device` with no KL_FRAMES would have sat on
  # a blocking --console forever waiting for an app that never exits.
  # ...and KL_SLINK_WAIT is the third: it is the Steam Link target's KL_FRAMES,
  # a bounded pump on the window path, and a run doing one still never exits on
  # its own — so --console would sit there until the app was quit by hand while
  # the log it should be pulling sat in the container.
  OPEN_ENDED=""
  [ -z "${KL_FRAMES:-}" ]     || OPEN_ENDED=1
  [ -z "${KL_SLINK_WAIT:-}" ] || OPEN_ENDED=1
  case "${KL_IMMERSIVE:-1}" in 0|no|off|false|"") ;; *) OPEN_ENDED=1 ;; esac
  if [ -z "$OPEN_ENDED" ]; then
    # P4's shape: --console blocks until the app terminates, which for a UI app
    # means until it is quit by hand. Fine for a run that finishes in seconds.
    xcrun devicectl device process launch --device "$DEVID" --console \
      --terminate-existing --environment-variables "$ENVJSON" "$BUNDLE_ID"
  else
    # A lifecycle run takes minutes and the app still never exits on its own, so
    # --console would just sit there waiting to be force-quit — and force-quitting
    # is indistinguishable from the run having finished. Launch detached and watch
    # the log instead: it is line-buffered precisely so it can be read live.
    # --terminate-existing is NOT optional. Without it, launching while the
    # app is already running does not start a new process — it ACTIVATES the
    # existing one, which keeps the environment it was launched with and the
    # binary it already mapped. The launch reports success either way. So a
    # rebuilt app with new knobs silently does not run, the log is not rewritten
    # (it belongs to the old process, which has already finished writing), and
    # what you are looking at in the headset is the PREVIOUS build.
    #
    # This cost a wrong conclusion: two runs of a new floor-test immersive space
    # reported "black" that were the previous run still on screen. The earlier
    # runs in the same session had relaunched correctly only because the process
    # happened to have died first, which is what made it look reproducible.
    xcrun devicectl device process launch --device "$DEVID" --terminate-existing \
      --environment-variables "$ENVJSON" "$BUNDLE_ID" | tail -2
    echo
    echo "    watching the log; it is done when it stops growing (Ctrl-C to stop)"
    LOCAL="${KL_LOG_OUT:-/tmp/klepton-device.log}"
    last=-1; same=0
    for _ in $(seq 1 "${KL_WATCH:-120}"); do
      sleep 5
      # Into a temp first, and never let a SHORTER log replace a longer one: a
      # run that dies is relaunched by the system, and the fresh process
      # truncates klepton-boot.log on its way up. Copying straight onto $LOCAL
      # therefore replaces the crash we care about with the clean boot that
      # followed it, a few seconds later — the evidence destroys itself while
      # the poller reports steadily shrinking line counts.
      TMPLOG="$LOCAL.new"
      xcrun devicectl device copy from --device "$DEVID" \
        --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
        --source Documents/klepton-boot.log --destination "$TMPLOG" >/dev/null 2>&1 || continue
      n=$(wc -l < "$TMPLOG" | tr -d ' ')
      prev=0; [ -f "$LOCAL" ] && prev=$(wc -l < "$LOCAL" | tr -d ' ')
      if [ "$n" -lt "$prev" ]; then
        mv "$TMPLOG" "$LOCAL.relaunch"
        echo "    app was relaunched ($n lines) — keeping the $prev-line log, new one in $LOCAL.relaunch"
        break
      fi
      mv "$TMPLOG" "$LOCAL"
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
xcodebuild -project "$PRODUCT.xcodeproj" -scheme "$PRODUCT" \
  -destination "platform=visionOS Simulator,id=$UDID" \
  -derivedDataPath "build/dd-sim-$KLT_NAME" CODE_SIGNING_ALLOWED=NO build 2>&1 \
  | grep -E 'error:|\*\* BUILD'
BUILD_RC=${PIPESTATUS[0]}
set -e
APP="build/dd-sim-$KLT_NAME/Build/Products/Debug-xrsimulator/$PRODUCT.app"
[ "$BUILD_RC" = 0 ] || { echo "!! build FAILED (see errors above) — not installing a stale app"; exit 1; }
[ -f "$APP/Info.plist" ] || { echo "!! no usable .app at $APP"; exit 1; }

echo "[4/5] installing + staging…"
xcrun simctl install "$UDID" "$APP"
# After install, not before: simctl rotates the data container on reinstall, so
# assets staged earlier end up in an orphaned one and the app reports them
# missing. Cost a confusing run once.
stage_if_needed "sim-$UDID"

echo "[5/5] launching…"
# `env`, not a bare prefix: bash only treats NAME=value as an assignment when it
# is literally there before expansion, so ${KL_FRAMES:+SIMCTL_CHILD_KL_FRAMES=…}
# is parsed as a *command* name and dies with "command not found".
SIMENV=(SIMCTL_CHILD_KL_AUTOBOOT=1)
for K in $FORWARD; do
  eval "V=\${$K:-}"
  [ -z "$V" ] || SIMENV+=("SIMCTL_CHILD_$K=$V")
done
env "${SIMENV[@]}" xcrun simctl launch --terminate-running-process "$UDID" "$BUNDLE_ID"
LOG="$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)/Documents/klepton-boot.log"
echo "    log: $LOG"
n=0; until [ -f "$LOG" ] || [ $n -ge 20 ]; do sleep 1; n=$((n+1)); done
sleep 10
LC_ALL=C grep -aE 'EXIT CRITERION|FAIL|translated dylib|natives registered|ids requested' "$LOG" || true
