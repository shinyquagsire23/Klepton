#!/bin/bash
# Build, install and run the STEAM LINK target on a physical Apple Vision Pro.
#
# The hybrid of the two wrappers, because neither alone fits this target on
# device. build_run_vpro.sh owns the device loop — build through visionos/run.sh,
# don't re-upload the assets, pull the log back, end with an answer — but its
# summary asks Unity's questions of a guest that is not Unity. build_run_host.sh
# asks the right questions — did the chain bind, did the doors open, where did it
# stop BY NAME — but drives `build/m_boot`, a HOST run. This script is the device
# loop with the Steam Link questions, and its flags are the host wrapper's flags
# restricted to the knobs the APP actually reads (kl_app.c / kl_driver.c /
# kl_slink.c):
#
#   ./build_run_slink_vpro.sh             # what a normal launch does: the 2D
#                                         #   shell in a window, pair, and the
#                                         #   app hands off to the OpenXR half
#                                         #   in the ImmersiveSpace itself
#   ./build_run_slink_vpro.sh --shell     # pin the SHELL front door, overriding
#                                         #   a carried session (kl_app.c: the
#                                         #   shell is already the default —
#                                         #   this wins over --sargs)
#   ./build_run_slink_vpro.sh --vr        # the VR front door DIRECTLY. It reads
#                                         #   its session from the environment
#                                         #   and leaves before its first frame
#                                         #   without one — pair with --sargs
#   ./build_run_slink_vpro.sh --sargs 'S' # hand the run a session (KL_SLINK_SARGS,
#                                         #   e.g. pasted from a host pairing);
#                                         #   carrying one selects the VR door
#   ./build_run_slink_vpro.sh --host <ip> # aim host discovery instead of the
#                                         #   broadcast sweep (KL_SLINK_HOST,
#                                         #   comma-separated for several)
#   ./build_run_slink_vpro.sh --wait 60   # seconds of looper pump (KL_SLINK_WAIT,
#                                         #   and the media/audio/XR/GL report is
#                                         #   written at the END of the pump). The
#                                         #   SHELL door runs unbounded without
#                                         #   it — it is being USED, not measured
#                                         #   — and this bounds it; the VR door
#                                         #   takes kl_driver_pump_default(),
#                                         #   which is 10 s, a measurement rather
#                                         #   than a session
#   ./build_run_slink_vpro.sh --immersive # open the ImmersiveSpace at launch.
#                                         #   Implied by --vr and --sargs: the
#                                         #   space is OFF by default for this
#                                         #   target (a shell run wants the
#                                         #   window it reports into), and the VR
#                                         #   door without one pumps a guest whose
#                                         #   picture has nowhere to go. The
#                                         #   2D->VR handoff opens it on its own
#   ./build_run_slink_vpro.sh --window    # window only: hold the space shut even
#                                         #   on the VR door
#   ./build_run_slink_vpro.sh --null      # the null GL driver instead of ANGLE
#   ./build_run_slink_vpro.sh --permissive# unimplemented JNI returns 0: scouting
#   ./build_run_slink_vpro.sh --trace-fs  # log every guest file op
#   ./build_run_slink_vpro.sh --trace-net # ...and every socket op
#   ./build_run_slink_vpro.sh --stage     # re-upload the assets + qt plugins
#   ./build_run_slink_vpro.sh --log       # pull + summarise the last run only
#
# What is deliberately NOT here, from build_run_host.sh's list: --gap, --main,
# --nofork and --no-handoff are `build/m_boot` knobs — KL_SLINK_HANDOFF is read
# only by mains/m_boot.c, whose shell RE-EXECS into the VR door. The app cannot
# re-exec; its two front doors share one process and the 2D->VR handoff is the
# app's own (kl_app.c, app_vrlink_handoff), so there is no host-side phase to
# isolate. There is no target argument either: on device the tree is the
# TARGET's (staged assets + embedded translations), not an argument.
#
# What the app decides for this target before its chain runs — kl_app.c,
# steamlink_policy() — so no flag has to: the topmost projection layer is the one
# composited, the eye texture is allocated 2x and capped at 3456, the display rate
# is pinned to 90 Hz (this client dereferences a null frametime container on a
# MEASURED rate) and the audio buffer is 240 ms (a network stream jitters through
# the 80 ms local-mixer default). Each is one env var away — KL_XR_CAPTURE_LAYER,
# KL_XR_EYE_SCALE / KL_XR_EYE_MAX, KL_DISPLAY_HZ, KL_AUDIO_LATENCY_MS — and
# run.sh forwards every KL_* it sees. Those defaults are the APP's alone;
# `build/m_boot` sets none of them, so a host run still passes them by hand.
#
# Anything already set in the environment wins, so a one-off knob the flags do
# not cover still works:
#
#   KL_SLINK_SIZE=1280x800 ./build_run_slink_vpro.sh
#
set -euo pipefail
cd "$(dirname "$0")"

# Pinned, not defaulted: the whole point of the wrapper is this target's
# questions, and a bare-word target argument would quietly turn it back into
# build_run_vpro.sh with the wrong summary.
export KLEPTON_TARGET=steamlink-vr

STAGE=""          # empty => KL_SKIP_STAGE=1 (assets persist across installs)
LOG_ONLY=""

while [ $# -gt 0 ]; do
  case "$1" in
    --shell)      export KL_SLINK_SHELL=1; shift ;;
    --vr)         export KL_SLINK_VR=1; shift ;;
    --sargs)      export KL_SLINK_SARGS="$2"; shift 2 ;;
    --host)       export KL_SLINK_HOST="$2"; shift 2 ;;
    --wait)       export KL_SLINK_WAIT="$2"; shift 2 ;;
    --immersive)  export KL_IMMERSIVE=1; shift ;;
    --window)     export KL_IMMERSIVE=0; shift ;;
    --null)       export KL_GLFB=0; shift ;;
    --permissive) export KL_PERMISSIVE=1; shift ;;
    --trace-fs)   export KL_TRACE_FS=1; shift ;;
    --trace-net)  export KL_TRACE_NET=1; shift ;;
    --stage)      STAGE=1; shift ;;
    --log)        LOG_ONLY=1; shift ;;
    -h|--help)    sed -n '2,80p' "$0"; exit 0 ;;
    *)            echo "unknown flag: $1 (try --help)" >&2; exit 2 ;;
  esac
done

# The VR door opens the ImmersiveSpace, because nothing else will. The app
# defaults the space OFF for this target — a shell run is read, typed into and
# waited on, and an immersive space that is black by construction hides the one
# surface with information on it — but that is the SHELL's default taken by a
# door that has a session and a picture. --window still wins — it sets the same
# variable — and so does a KL_IMMERSIVE already in the environment.
if [ -z "${KL_IMMERSIVE:-}" ] \
   && { [ -n "${KL_SLINK_VR:-}" ] || [ -n "${KL_SLINK_SARGS:-}" ]; } \
   && [ -z "${KL_SLINK_SHELL:-}" ]; then
  export KL_IMMERSIVE=1
fi

# Resolved the way build_run_vpro.sh resolves it, for the same reason: the
# bundle id is what the log is pulled out of, so it has to come from the table
# rather than be restated here.
KLT_VARS=$(python3 visionos/targets.py "$KLEPTON_TARGET") || exit 1
eval "$KLT_VARS"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
LOCAL_LOG="${KL_LOG_OUT:-/tmp/klepton-$KLT_NAME.log}"

# Device discovery, shared shape with build_run_vpro.sh: JSON, because device
# names contain non-breaking spaces and column parsing picks the wrong field.
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

# Pull the log, but never let a shorter one overwrite a longer one — the device
# relaunches a dead app and the fresh process truncates klepton-boot.log, so
# the interesting log destroys itself seconds after it is written. Same rule as
# build_run_vpro.sh, verbatim.
pull_log() {
  local tmp; tmp="$(mktemp)"
  xcrun devicectl device copy from --device "$DEVID" \
    --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
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

# LC_ALL=C and -a everywhere: the log carries guest bytes, and without both grep
# bails with "illegal byte sequence" and silently shows nothing.
g() { LC_ALL=C grep -a "$@"; }

# The summary is the point of the wrapper, and it is build_run_slink.sh's
# questions asked of build_run_vpro.sh's log: whose run is this, which front
# door did it open, did the chain bind, did the app start, and where did it
# stop BY NAME.
summarise() {
  [ -f "$LOCAL_LOG" ] || { echo "!! no log at $LOCAL_LOG"; return 1; }
  echo
  echo "──────── $LOCAL_LOG ($(wc -l < "$LOCAL_LOG" | tr -d ' ') lines) ────────"

  # Whose run is this? The first heartbeat is the run's own start time. Read it
  # as a device-clock reading, not this Mac's — the headset keeps its own zone.
  echo "  started : $(g -m1 '^\[hb\]' "$LOCAL_LOG" | awk '{print $2}')  (device clock; this Mac says $(date +%H:%M:%S))"

  # Which door, and why — kl_app_configure prints the decision with the three
  # inputs, because an app launched from the Home View has no environment and a
  # run that silently took the shell is indistinguishable from a broken handoff.
  echo "  -- front door --"
  g -E 'front door' "$LOCAL_LOG" | sed 's/^ *//; s/^/    /' | head -6 || true

  echo "  -- chain --"
  g -E 'chain mapped|unique unresolved|data range\(s\) inside' "$LOCAL_LOG" \
    | sed 's/^ *//; s/^/    /' || true
  g -E 'JNI_OnLoad returned|static Java_\* natives|SDL_main=|EXIT CRITERION|nativeSetupJNI returned' \
    "$LOCAL_LOG" | sed 's/^ *//; s/^/    /' || true
  g -E '^  (natives registered|ids requested|classes found):' "$LOCAL_LOG" \
    | sed 's/^ *//; s/^/    /' || true

  # The work list, when the run printed one. Everything still unresolved after
  # the guest libraries satisfied each other.
  if g -q '=== shim gap' "$LOCAL_LOG"; then
    echo "  -- unresolved (the work list) --"
    sed -n '/=== shim gap/,/unique unresolved/p' "$LOCAL_LOG" \
      | g -vE '=== shim gap|unique unresolved' | g -v '^ *$' \
      | sed 's/^ */    /' | head -40 || true
  fi

  # Did the app start, and did it draw. `SDL_main returned` is the guest
  # EXITING — with no Steam host on the network that is this app's normal path,
  # so a black window here is the guest not running, not the compositor.
  echo "  -- the app --"
  g -E 'Audio initialized|Video initialized|Desktop mode|Created .* renderer|Initialized player|MESSAGEBOX|2D -> VR handoff|guest is (MONO|STEREO)' \
    "$LOCAL_LOG" | sed 's/^\[[0-9]*\/SDL\/APP\] //; s/^\[jni\] //; s/^ *//; s/^/    /' | awk '!seen[$0]++' | head -20 || true
  if g -q 'nativeRunMain returned\|SDL_main returned' "$LOCAL_LOG"; then
    echo "    NOTE: SDL_main RETURNED — the guest exited on its own (the no-host path)."
  fi

  echo "  -- compositor / graphics --"
  # [ovrp] belongs here now: the display rate is PINNED for this target rather
  # than measured, and which of the two happened is one line.
  g -E '\[glfb\]|\[cp\]|present\]|\[ovrp\] display frequency' "$LOCAL_LOG" \
    | sed 's/^/    /' | head -40 || true

  # The stream itself, which is what a run of this target is FOR — the decoder's
  # own counts and the audio's underruns, both written into the end-of-pump
  # report. A stream that arrives and does not decode looks exactly like a
  # compositor problem from the picture alone.
  echo "  -- the stream --"
  g -E 'first frame published|codec "|reader:|submitted, [0-9]+ decoded|parameter sets:|CoreAudio:|\[aaudio\]|\[au\] (restart|session|no render callback)' \
    "$LOCAL_LOG" | sed 's/^ *//; s/^/    /' | awk '!seen[$0]++' | head -12 || true

  echo "  -- how it ended --"
  # The heartbeat separates the three failures that otherwise look identical:
  # a timestamp gap is a suspension, a log that simply ends is a kill, and a
  # heartbeat still ticking while the guest goes quiet is a stuck guest thread.
  echo "    last heartbeat: $(g '^\[hb\]' "$LOCAL_LOG" | tail -1)"
  # One alternation, deduped: a line matches both its own pattern and the
  # generic "fatal:" wrapper, and one stop reported twice reads as two.
  local stops
  stops=$(g -E 'called unresolved import|UNIMPLEMENTED JNIEnv slot|no host implementation for|no host value for field|klepton\] fatal:' \
          "$LOCAL_LOG" | awk '!seen[$0]++' | head -3)
  [ -z "$stops" ] || echo "$stops" | sed 's/^/    STOPPED: /'
  if g -q 'klepton\] fault' "$LOCAL_LOG"; then
    echo "    FAULT:"
    g -A14 'klepton\] fault' "$LOCAL_LOG" | sed 's/^/      /'
  fi
  echo "    last output   : $(g -v '^\[hb\]' "$LOCAL_LOG" | g -v '^ *$' | tail -1 | cut -c1-120)"
  echo
}

if [ -n "$LOG_ONLY" ]; then
  # A STREAMED run's log lives in $LOCAL_LOG and nowhere else — the app ran
  # with KL_LOG_FILE=0, so the container's klepton-boot.log is some OLDER
  # run's. Pulling here anyway let keep-the-longest replace the newest
  # evidence with an 11k-line relic; the sentinel is exactly the record of
  # which world we are in, so honour it in this mode too.
  if [ -f "$LOCAL_LOG.streamed" ]; then
    echo "    (last run streamed into $LOCAL_LOG — summarising it; the container's"
    echo "     file is older. For a Home-View launch's log: rm $LOCAL_LOG.streamed)"
  else
    pull_log || { echo "!! could not copy the log off $DEVID"; exit 1; }
  fi
  summarise
  exit 0
fi

[ -n "$STAGE" ] || export KL_SKIP_STAGE=1
export KL_ALARM="${KL_ALARM:-180}"
export KL_LOG_OUT="$LOCAL_LOG"

echo "target    : $KLT_NAME ($BUNDLE_ID, $KLT_PRODUCT.app)"
echo "device    : $DEVID"
echo "knobs     : ${KL_SLINK_SHELL:+KL_SLINK_SHELL=1 }${KL_SLINK_VR:+KL_SLINK_VR=1 }${KL_SLINK_SARGS:+KL_SLINK_SARGS=(carried) }${KL_SLINK_HOST:+KL_SLINK_HOST=$KL_SLINK_HOST }${KL_SLINK_WAIT:+KL_SLINK_WAIT=$KL_SLINK_WAIT }${KL_IMMERSIVE:+KL_IMMERSIVE=$KL_IMMERSIVE }${KL_GLFB:+KL_GLFB=$KL_GLFB }${KL_PERMISSIVE:+KL_PERMISSIVE=1 }${KL_TRACE_FS:+KL_TRACE_FS=1 }${KL_TRACE_NET:+KL_TRACE_NET=1 }KL_ALARM=$KL_ALARM${KL_SKIP_STAGE:+ (assets not re-staged)}"
echo "            (unset knobs take the app's defaults: the 2D shell, ANGLE, the"
echo "             space opened by the handoff rather than at launch, and the"
echo "             Steam Link policy kl_app.c sets — 90 Hz, 240 ms audio, topmost"
echo "             layer, 2x eye. run.sh forwards every KL_* it sees)"
echo

# Cleared before, not after: the sentinel says how THIS run produced its log,
# and a leftover from the previous one would make a KL_CONSOLE=0 run skip the
# pull it needs and then summarise the older run's file.
rm -f "$LOCAL_LOG.streamed"

visionos/run.sh device

# run.sh polls or streams the log while the run is live; this is the final
# read. A streamed log is already this run's — pulling the container's file on
# top of it would replace this run's output with an older run's.
if [ -f "$LOCAL_LOG.streamed" ]; then
  echo "    (streamed live over --console; the container's log is not this run's)"
else
  pull_log || true
fi
summarise
