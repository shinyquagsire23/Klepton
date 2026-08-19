#!/bin/bash
# Build and run a guest on macOS, and end with an answer rather than a path to a
# log.
#
# The counterpart to build_run_vpro.sh, which wraps a device launch. Everything
# unimplemented fails BY NAME, so the stop is the work item, and this script's
# job is to pull that one line out of a few hundred:
#
#   1. did the chain bind, and what is still unresolved
#   2. how far the guest got
#   3. where it stopped, by name
#
#   ./build_run_host.sh                    # the default target (beatsaber)
#   ./build_run_host.sh steamlink-vr       # any target; --list names them
#   ./build_run_host.sh re4 --alarm 300 --timeout 400
#
#   --gap          the shim work list ONLY: map and relocate everything, print
#                  what is unresolved, stop before DT_INIT_ARRAY. Seconds, and
#                  the right first command after adding shims
#   --lifecycle    drive the guest's lifecycle after the boot gate (Unity only;
#                  the other doors start their guest as part of the run)
#   --frames N     bound a Unity guest's frame count
#   --permissive   unimplemented JNI calls return 0 instead of aborting, so ONE
#                  run collects the whole batch. Scouting only — the guest then
#                  carries on with answers we invented
#   --gl           ANGLE instead of the null GL driver
#   --view         ...and put it in a window (kl_view.c). Runs until you close
#                  it: no timeout, because there is no deadline on a person
#                  looking at something
#   --nofork       run in-process (required under lldb: macOS lldb follows
#                  neither fork nor exec)
#   --trace-fs     log every guest file op ('=fail' via env)
#   --log          re-summarise the last run, build nothing
#
# Steam Link only — the one target whose front door is a choice:
#   --shell        the 2D configuration frontend (libshell + Qt6) instead of the
#                  streaming client. Once the host authorizes, this RE-EXECS
#                  itself into the OpenXR front door carrying the session — one
#                  run, pairing to stream
#   --vr           the OpenXR front door directly (needs a session: KL_SLINK_SARGS)
#   --main         drive onCreate on into the guest's own main/frame loop.
#                  Implies --gl (the null driver stops at
#                  glCheckFramebufferStatus by design)
#   --no-handoff   stop at the handoff and print the session instead of re-exec'ing
#
# Anything already in the environment wins, so a one-off knob no flag covers
# still works:
#
#   KL_TRACE_NET=1 ./build_run_host.sh steamlink-vr --shell
#
set -uo pipefail
cd "$(dirname "$0")"

TARGET=""
TIMEOUT="${KL_TIMEOUT:-10}"
LOG_ONLY=""
VIEW=""

while [ $# -gt 0 ]; do
  case "$1" in
    --list)        python3 visionos/targets.py --list; exit 0 ;;
    --gap)         export KL_GAP_ONLY=1; export KL_NOFORK=1; shift ;;
    --lifecycle)   export KL_LIFECYCLE=1; shift ;;
    --frames)      export KL_FRAMES="$2"; export KL_LIFECYCLE=1; shift 2 ;;
    --permissive)  export KL_PERMISSIVE=1; shift ;;
    --gl)          export KL_GLFB=1; shift ;;
    # --main and --view both imply the real GL path: the null driver stops at
    # glCheckFramebufferStatus, so a guest's own main cannot be reached on it.
    --main)        export KL_SLINK_MAIN=1; export KL_GLFB=1; export KL_NOFORK=1; shift ;;
    --shell)       export KL_SLINK_SHELL=1; TARGET="${TARGET:-steamlink-vr}"; shift ;;
    --vr)          export KL_SLINK_VR=1;    TARGET="${TARGET:-steamlink-vr}"; shift ;;
    --no-handoff)  export KL_SLINK_HANDOFF=0; shift ;;
    --view)        export KL_VIEW=1; export KL_GLFB=1; export KL_NOFORK=1
                   VIEW=1; shift ;;
    --nofork)      export KL_NOFORK=1; shift ;;
    --trace-fs)    export KL_TRACE_FS=1; shift ;;
    --trace-net)   export KL_TRACE_NET=1; shift ;;
    --alarm)       export KL_ALARM="$2"; shift 2 ;;
    --timeout)     TIMEOUT="$2"; shift 2 ;;
    --log)         LOG_ONLY=1; shift ;;
    -h|--help)     sed -n '2,47p' "$0"; exit 0 ;;
    -*)            echo "unknown flag: $1 (try --help)" >&2; exit 2 ;;
    # A bare word is the TARGET. One only: "which guest did that run" is not a
    # question worth having to ask of the log afterwards.
    *)             [ -z "$TARGET" ] || {
                     echo "!! two targets given ($TARGET and $1)" >&2; exit 2; }
                   TARGET="$1"; shift ;;
  esac
done

TARGET="${TARGET:-${KLEPTON_TARGET:-beatsaber}}"
# Resolved through targets.py so an unknown name stops here rather than after a
# build, and so the log is named after the target: two targets writing one path
# means the second run's summary describes the first.
python3 visionos/targets.py "$TARGET" name > /dev/null || exit 1
LOG="${KL_LOG_OUT:-/tmp/klepton-host-$TARGET.log}"

# LC_ALL=C everywhere below, and grep -a: the log carries guest bytes, and
# without both grep bails with "illegal byte sequence" and silently shows
# nothing — which reads as "that never happened".
g() { LC_ALL=C grep -a "$@"; }

summarise() {
  [ -f "$LOG" ] || { echo "!! no log at $LOG"; return 1; }
  echo
  echo "──────── $LOG ($(wc -l < "$LOG" | tr -d ' ') lines) ────────"

  echo "  -- chain --"
  g -E '^  lib.*\.so +[0-9@]' "$LOG" | sed 's/^ */    /' || true
  g -E 'chain mapped|unique unresolved|data range\(s\) inside' "$LOG" \
    | sed 's/^ *//; s/^/    /' || true

  # The work list: everything still unresolved after the guest libraries have
  # satisfied each other — NOT what t_load reports for one library in isolation.
  #
  # The range ends on the count line rather than on a blank one: the symbol
  # columns are %-28s padded, so the "blank" line separating them is spaces and
  # /^$/ never matches it.
  if g -q '=== shim gap' "$LOG"; then
    echo "  -- unresolved (the work list) --"
    sed -n '/=== shim gap/,/unique unresolved/p' "$LOG" \
      | g -vE '=== shim gap|unique unresolved' | g -v '^ *$' \
      | sed 's/^ */    /' | head -40 || true
  fi

  echo "  -- the guest --"
  g -E 'JNI_OnLoad returned|static Java_\* natives|EXIT CRITERION|NativeLoader.load returned|nativeSetupJNI returned|pumped ' \
    "$LOG" | sed 's/^ *//; s/^/    /' || true
  g -E '^  (natives registered|ids requested|classes found):' "$LOG" \
    | sed 's/^ *//; s/^/    /' || true

  # Did it draw? SWAPS is the honest number: no eglSwapBuffers and no lit eye
  # means the guest never presented a frame, so nothing downstream — sink,
  # compositor, window — can be at fault.
  local lit frames
  lit=$(g -oE 'lit=[0-9]+|[0-9]+/[0-9]+ lit' "$LOG" | g -vE '^lit=0$|^0/' | head -1)
  frames=$(g -oE 'frame [0-9]+' "$LOG" | tail -1 | awk '{print $2+0}')
  [ -z "$frames$lit" ] || echo "    frames drawn: ${frames:-?}${lit:+, $lit}"
  g -E 'MESSAGEBOX' "$LOG" | sed 's/^\[jni\] /    /' || true
  if g -q 'nativeRunMain returned' "$LOG"; then
    echo "    NOTE: the guest's main RETURNED — it exited on its own. For the"
    echo "          Steam Link client with no host on the network that is the"
    echo "          normal path, and it exits before drawing anything."
  fi

  # ---- where it stopped, by name ----
  echo "  -- how it ended --"
  # One alternation rather than a loop per pattern, and deduped: a single line
  # matches both its specific pattern and the generic "fatal:" wrapper, and
  # reporting the same stop twice makes it look like two different failures.
  local found=""
  local stops
  stops=$(g -E 'called unresolved import|UNIMPLEMENTED JNIEnv slot|no host implementation for|no host value for field|klepton\] fatal:' \
          "$LOG" | awk '!seen[$0]++' | head -3)
  if [ -n "$stops" ]; then
    echo "$stops" | sed 's/^/    STOPPED: /'
    found=1
  fi
  if g -q 'klepton\] fault' "$LOG"; then
    echo "    FAULT:"
    g -A12 'klepton\] fault' "$LOG" | sed 's/^/      /'
    found=1
  fi
  # A stop with no named cause is the interesting case, not the boring one: it
  # means the guest died somewhere we have no instrument for. Say so explicitly
  # rather than printing nothing, which reads as a clean run.
  if [ -z "$found" ]; then
    if [ -n "${KL_GAP_ONLY:-}" ]; then
      echo "    (--gap: stopped at the shim gap on purpose — nothing was run)"
    elif [ "${rc:-0}" -eq 0 ]; then
      echo "    clean: no unimplemented call was reached"
    else
      echo "    (no named stop — the guest died somewhere with no instrument;" \
           "try --nofork under lldb)"
    fi
  fi
  echo "    last output: $(g -v '^ *$' "$LOG" | tail -1 | cut -c1-140)"
  echo
}

if [ -n "$LOG_ONLY" ]; then summarise; exit 0; fi

# Gate on the BUILD's exit status, not on the binary existing. A failed
# incremental build leaves the last good ./build/m_boot in place, so running
# anyway would test the previous code and report it as the new code's result.
echo "building  : build/m_boot"
BUILDLOG="${LOG%.log}-build.log"
make build/m_boot > "$BUILDLOG" 2>&1
build_rc=$?
g -E 'error|warning:' "$BUILDLOG" | sed 's/^/  /' | head -20
if [ $build_rc -ne 0 ]; then
  echo "!! build failed (see $BUILDLOG) — NOT running the previous binary"
  exit 1
fi

KNOBS="${KL_SLINK_SHELL:+KL_SLINK_SHELL=1 }${KL_SLINK_VR:+KL_SLINK_VR=1 }${KL_SLINK_MAIN:+KL_SLINK_MAIN=1 }${KL_LIFECYCLE:+KL_LIFECYCLE=1 }${KL_FRAMES:+KL_FRAMES=$KL_FRAMES }${KL_GAP_ONLY:+KL_GAP_ONLY=1 }${KL_PERMISSIVE:+KL_PERMISSIVE=1 }${KL_GLFB:+KL_GLFB=$KL_GLFB }${KL_NOFORK:+KL_NOFORK=1 }${KL_ALARM:+KL_ALARM=$KL_ALARM }${KL_TRACE_FS:+KL_TRACE_FS=1 }${KL_TRACE_NET:+KL_TRACE_NET=1 }"
echo "target    : $TARGET"
echo "knobs     : ${KNOBS:-(none — the default run)}"
echo "log       : $LOG"

# `script -q /dev/null` gives the child a pty, and the redirect sends it to a
# FILE. Both matter: with stdout a plain file stdio goes fully buffered, and a
# child that dies on a signal rather than through kl_fatal_prepare() loses the
# entire buffer — a dozen lines and no report, which reads as a much earlier
# failure than actually happened. A pipe straight to head/sed is the other half
# of the same trap: the child forks, and the report arrives out of order.
if [ -n "$VIEW" ]; then
  # No timeout: the run ends when the window is closed. The window IS the
  # output here, so the log is for afterwards rather than for watching.
  echo "            (window open — close it to end the run)"
  script -q /dev/null ./build/m_boot "$TARGET" 2>&1 | tee "$LOG"
  rc=${PIPESTATUS[0]}
else
  timeout "$TIMEOUT" script -q /dev/null ./build/m_boot "$TARGET" 2>&1 | tee "$LOG"
  rc=${PIPESTATUS[0]}
fi
echo
case $rc in
  0)   echo "exit      : 0" ;;
  124) echo "exit      : TIMEOUT after ${TIMEOUT}s — the guest is blocked on something;" \
            "raise --alarm to let the watchdog report what, or --timeout to wait longer" ;;
  *)   echo "exit      : $rc$([ $rc -gt 128 ] && echo "  (signal $((rc - 128)))")" ;;
esac
summarise
exit $rc
