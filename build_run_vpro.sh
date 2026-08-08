#!/bin/bash
# Build, install and run Klepton on a physical Apple Vision Pro — the quick loop.
#
# This is a thin, opinionated wrapper over visionos/run.sh: that script owns the
# build/install/stage/launch mechanics and stays the general one. This one adds
# the two things a quick iteration wants — not re-uploading the 2.2 GB of assets,
# and reading the log back so the run ends with an answer rather than a file path
# — plus short flags for the diagnostic shapes.
#
# It deliberately does NOT turn on immersive or ANGLE: those are the app's own
# defaults now, so what you get here is what you get tapping the icon.
#
#   ./build_run_vpro.sh                 # what a normal launch does: immersive +
#                                       #   ANGLE, autoboot, frames until stopped
#   ./build_run_vpro.sh --frames 30     # bound the guest's frame count
#   ./build_run_vpro.sh --p4            # P4's shape: boot to initJni, no compositor
#   ./build_run_vpro.sh --window --frames 300
#                                       # the lifecycle in the window, no immersive
#                                       #   space. --window ALONE is just --p4: the
#                                       #   window path runs the lifecycle only when
#                                       #   KL_FRAMES says how many frames to pump
#   ./build_run_vpro.sh --null          # the null GL driver instead of ANGLE
#   ./build_run_vpro.sh --stage         # re-upload the 2.2 GB of assets (first run only)
#   ./build_run_vpro.sh --sync          # drive the guest on the compositor thread
#   ./build_run_vpro.sh --quest-fov     # A/B: Quest 2's 90°/72 Hz, not the display's
#   ./build_run_vpro.sh --log           # pull + summarise the last run, build nothing
#
# Anything already set in the environment wins, so a one-off knob the flags do
# not cover still works:
#
#   KL_PERMISSIVE=1 ./build_run_vpro.sh --frames 60
#
set -euo pipefail
cd "$(dirname "$0")"

# The app's own defaults are now "immersive, with ANGLE, booting on its own"
# (KleptonApp.swift / kl_app_configure), so this script does not have to turn any
# of that on — it only has to be able to turn it OFF. Hence the empty defaults
# and the explicit =0 in the flags below: a flag that merely omitted a knob used
# to disable it and now would not.
FRAMES=""
IMMERSIVE=""      # unset => the app's default (on)
GLFB=""           # unset => the app's default (on)
STAGE=""          # empty => KL_SKIP_STAGE=1 (assets persist across installs)
LOG_ONLY=""
LOCAL_LOG="${KL_LOG_OUT:-/tmp/klepton-device.log}"

while [ $# -gt 0 ]; do
  case "$1" in
    --frames)     FRAMES="$2"; shift 2 ;;
    --p4)         FRAMES=""; IMMERSIVE=0; shift ;;    # boot only — P4's measurement
    --window)     IMMERSIVE=0; shift ;;
    --null)       GLFB=0; shift ;;
    --stage)      STAGE=1; shift ;;
    --sync)       export KL_SYNC_GUEST=1; shift ;;
    --quest-fov)  export KL_OVRP_QUEST_FOV=1; shift ;;
    --log)        LOG_ONLY=1; shift ;;
    -h|--help)    sed -n '2,30p' "$0"; exit 0 ;;
    *)            echo "unknown flag: $1 (try --help)" >&2; exit 2 ;;
  esac
done

# Device discovery, so --log works without a build and so the summary below can
# name the device. Same JSON route as run.sh: device names contain non-breaking
# spaces, which makes column parsing pick the wrong field.
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
export KLEPTON_DEVICE="$DEVID"

# Pull the log, but never let a shorter one overwrite a longer one — keep the
# long one and park the new one alongside.
#
# The app is RELAUNCHED after it dies here, and the fresh process truncates
# klepton-boot.log on the way up. So the interesting log — the one belonging to
# the run that failed — is destroyed a few seconds after it is written, and the
# poller happily replaces a 701-line crash with a 200-line clean boot. That is
# not a stale-log problem (the file really is current), it is the opposite: the
# evidence is overwritten by something newer and less interesting.
pull_log() {
  local tmp; tmp="$(mktemp)"
  xcrun devicectl device copy from --device "$DEVID" \
    --domain-type appDataContainer --domain-identifier "${KLEPTON_BUNDLE_ID:-dev.klepton.app}" \
    --source Documents/klepton-boot.log --destination "$tmp" >/dev/null 2>&1 || { rm -f "$tmp"; return 1; }
  local new old
  new=$(wc -l < "$tmp" | tr -d ' ')
  old=0; [ -f "$LOCAL_LOG" ] && old=$(wc -l < "$LOCAL_LOG" | tr -d ' ')
  if [ "$new" -lt "$old" ] && [ -n "${KL_KEEP_LONGEST:-1}" ]; then
    mv "$tmp" "$LOCAL_LOG.relaunch"
    echo "    (device relaunched the app: its $new-line log is in $LOCAL_LOG.relaunch;" \
         "keeping the $old-line one)" >&2
  else
    mv "$tmp" "$LOCAL_LOG"
  fi
}

# The summary is the point of the wrapper. Three questions, in the order they
# get asked of every run: did the guest boot, did the compositor come up, and
# how did it end. LC_ALL=C throughout — the log carries guest bytes and grep
# otherwise bails with "illegal byte sequence" and shows nothing.
summarise() {
  [ -f "$LOCAL_LOG" ] || { echo "!! no log at $LOCAL_LOG"; return 1; }
  echo
  echo "──────── $LOCAL_LOG ($(wc -l < "$LOCAL_LOG" | tr -d ' ') lines) ────────"

  # Whose run is this? A device log that looks stalled is often simply the
  # PREVIOUS run's file, left behind because the launch never happened. The
  # first heartbeat is the run's own start time and settles it in one line.
  #
  # Compare the two as a DELTA, not as clock readings: the headset keeps its own
  # timezone and is currently an hour behind this Mac, so a run that started ten
  # seconds ago prints an hour earlier than "now" and reads as an hour-old file.
  echo "  started : $(LC_ALL=C grep -am1 '^\[hb\]' "$LOCAL_LOG" | awk '{print $2}')  (device clock; this Mac says $(date +%H:%M:%S))"

  echo "  -- boot --"
  LC_ALL=C grep -aE 'EXIT CRITERION|classes:|natives registered|ids requested|guard verified' \
    "$LOCAL_LOG" | sed 's/^/    /' || true

  echo "  -- compositor / graphics --"
  LC_ALL=C grep -aE '\[glfb\]|\[cp\]' "$LOCAL_LOG" | sed 's/^/    /' | head -40 || true

  echo "  -- how it ended --"
  # The heartbeat separates the three failures that otherwise look identical:
  # a timestamp gap is a suspension, a log that simply ends is a kill, and a
  # heartbeat still ticking while the guest goes quiet is a stuck guest thread.
  echo "    last heartbeat: $(LC_ALL=C grep -a '^\[hb\]' "$LOCAL_LOG" | tail -1)"
  echo "    last output   : $(LC_ALL=C grep -av '^\[hb\]' "$LOCAL_LOG" | grep -av '^ *$' | tail -1 | cut -c1-120)"
  if LC_ALL=C grep -aq 'klepton\] fault' "$LOCAL_LOG"; then
    echo "    FAULT:"
    LC_ALL=C grep -a -A14 'klepton\] fault' "$LOCAL_LOG" | sed 's/^/      /'
  fi
  echo
}

if [ -n "$LOG_ONLY" ]; then
  pull_log || { echo "!! could not copy the log off $DEVID"; exit 1; }
  summarise
  exit 0
fi

# Knobs, exported for run.sh's FORWARD list. Set with :=-style defaults so an
# environment that already has one keeps it.
[ -z "$FRAMES" ]    || export KL_FRAMES="${KL_FRAMES:-$FRAMES}"
[ -z "$IMMERSIVE" ] || export KL_IMMERSIVE="${KL_IMMERSIVE:-$IMMERSIVE}"
[ -z "$GLFB" ]      || export KL_GLFB="${KL_GLFB:-$GLFB}"
[ -n "$STAGE" ]     || export KL_SKIP_STAGE=1
export KL_ALARM="${KL_ALARM:-180}"
export KL_LOG_OUT="$LOCAL_LOG"

echo "device    : $DEVID"
echo "knobs     : ${KL_FRAMES:+KL_FRAMES=$KL_FRAMES }${KL_IMMERSIVE:+KL_IMMERSIVE=$KL_IMMERSIVE }${KL_GLFB:+KL_GLFB=$KL_GLFB }KL_ALARM=$KL_ALARM${KL_SYNC_GUEST:+ KL_SYNC_GUEST=1}${KL_OVRP_QUEST_FOV:+ KL_OVRP_QUEST_FOV=1}${KL_SKIP_STAGE:+ (assets not re-staged)}"
echo "            (unset knobs take the app's defaults: immersive + ANGLE, autoboot)"
echo

visionos/run.sh device

# run.sh polls the log itself while the run is live; this is the final read, and
# it re-pulls because the run may have written more after the poller gave up.
pull_log || true
summarise
