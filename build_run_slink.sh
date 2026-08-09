#!/bin/bash
# Build and run the Steam Link target on macOS — the SL-1 loop (PLANNING §11.6).
#
# The counterpart to build_run_vpro.sh: that one wraps a device launch, this one
# wraps `make slink`. Both exist for the same reason — a run should end with an
# answer rather than a path to a log — and the summary here asks the three
# questions this target's bring-up loop asks of every run:
#
#   1. did the seven-library chain bind, and what is still unresolved
#   2. did SL-1 still pass (JNI_OnLoad, 68 natives, SDL_main)
#   3. where did it stop, BY NAME
#
# (3) is the whole M4 method applied to the second target: everything
# unimplemented fails by name, so the stop is the work item. This script's job
# is to pull that one line out of a few hundred.
#
#   ./build_run_slink.sh              # build + full run + summary
#   ./build_run_slink.sh --gap        # the shim work list ONLY: map and relocate
#                                     #   everything, print what is unresolved, stop
#                                     #   before DT_INIT_ARRAY. Seconds, and it is
#                                     #   the right first command after adding shims
#   ./build_run_slink.sh --permissive # unimplemented JNI calls return 0 instead of
#                                     #   aborting, so ONE run collects the whole
#                                     #   batch. Scouting only — the guest then
#                                     #   carries on with answers we invented
#   ./build_run_slink.sh --gl         # ANGLE instead of the null GL driver. Steam
#                                     #   Link is GLES2-only, so this is the real
#                                     #   path once anything draws
#   ./build_run_slink.sh --main       # SL-2: drive onCreate through nativeRunMain
#                                     #   into SDL_main, so the app actually starts.
#                                     #   Implies --gl (the null driver stops at
#                                     #   glCheckFramebufferStatus by design)
#   ./build_run_slink.sh --view       # ...and put it in a window (kl_view.c). Steam
#                                     #   Link is a FLAT app, so the viewer is its
#                                     #   real output device, not a debugging aid.
#                                     #   Runs until you close the window: no
#                                     #   timeout, because there is no deadline on
#                                     #   a person looking at something
#   ./build_run_slink.sh --nofork     # run in-process (required under lldb: macOS
#                                     #   lldb follows neither fork nor exec)
#   ./build_run_slink.sh --trace-fs   # log every guest file op ('=fail' via env)
#   ./build_run_slink.sh --log        # re-summarise the last run, build nothing
#
# Anything already in the environment wins, so a one-off knob no flag covers
# still works:
#
#   KL_TRACE_NET=1 ./build_run_slink.sh
#
set -uo pipefail
cd "$(dirname "$0")"

LIBDIR="${KL_SLINK_LIBDIR:-steamlink-android/lib/arm64-v8a}"
LOG="${KL_LOG_OUT:-/tmp/slink.log}"
TIMEOUT="${KL_TIMEOUT:-10}"
LOG_ONLY=""
VIEW=""
SHELL_MODE=""
LIBDIR_SET=""

while [ $# -gt 0 ]; do
  case "$1" in
    --gap)         export KL_GAP_ONLY=1; export KL_NOFORK=1; shift ;;
    --permissive)  export KL_PERMISSIVE=1; shift ;;
    --gl)          export KL_GLFB=1; shift ;;
    # --main and --view both imply the real GL path: the null driver stops at
    # glCheckFramebufferStatus, so SL-2 cannot be reached on it at all.
    --main)        export KL_SLINK_MAIN=1; export KL_GLFB=1; export KL_NOFORK=1; shift ;;
    # The other front door: the 2D configuration frontend (libshell + Qt6)
    # instead of the streaming client. VR APK only, so it selects that LIBDIR
    # unless one was given explicitly.
    --shell)       export KL_SLINK_SHELL=1; SHELL_MODE=1; shift ;;
    --view)        export KL_VIEW=1; export KL_GLFB=1; export KL_NOFORK=1
                   VIEW=1; shift ;;
    --nofork)      export KL_NOFORK=1; shift ;;
    --trace-fs)    export KL_TRACE_FS=1; shift ;;
    --trace-net)   export KL_TRACE_NET=1; shift ;;
    --alarm)       export KL_ALARM="$2"; shift 2 ;;
    --timeout)     TIMEOUT="$2"; shift 2 ;;
    --libdir)      LIBDIR="$2"; LIBDIR_SET=1; shift 2 ;;
    --log)         LOG_ONLY=1; shift ;;
    -h|--help)     sed -n '2,35p' "$0"; exit 0 ;;
    *)             echo "unknown flag: $1 (try --help)" >&2; exit 2 ;;
  esac
done

# --shell only exists on the VR APK: the old one ships Qt5 + the stock
# qtforandroid QPA, the VR one ships Qt6 + Valve's own qvirtual. Selecting the
# tree here rather than making the caller remember it, but never overriding an
# explicit --libdir.
if [ -n "$SHELL_MODE" ] && [ -z "$LIBDIR_SET" ]; then
  LIBDIR="steamlink-vr/lib/arm64-v8a"
fi

# LC_ALL=C everywhere below, and grep -a: the log carries guest bytes, and
# without both grep bails with "illegal byte sequence" and silently shows
# nothing — which reads as "that never happened".
g() { LC_ALL=C grep -a "$@"; }

summarise() {
  [ -f "$LOG" ] || { echo "!! no log at $LOG"; return 1; }
  echo
  echo "──────── $LOG ($(wc -l < "$LOG" | tr -d ' ') lines) ────────"

  echo "  -- chain --"
  g -E '^  lib.*\.so +[0-9]' "$LOG" | sed 's/^ */    /' || true
  g -E 'chain mapped|unique unresolved|data range\(s\) inside' "$LOG" \
    | sed 's/^ *//; s/^/    /' || true

  # The work list. Everything still unresolved after the guest libraries have
  # satisfied each other — NOT what t_load reports for one library in isolation,
  # which is mostly SDL3 symbols the guest itself provides.
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

  # Skipped under --gap: the run stops before any of it happens, and an empty
  # heading reads as a failure rather than as a question not asked.
  if g -q 'phase 2' "$LOG"; then
    echo "  -- SL-1 --"
    g -E 'JNI_OnLoad returned|static Java_\* natives|SDL_main=|EXIT CRITERION|nativeSetupJNI returned' \
      "$LOG" | sed 's/^ *//; s/^/    /' || true
    g -E '^  (natives registered|ids requested|classes found):' "$LOG" \
      | sed 's/^ *//; s/^/    /' || true
  fi

  # ---- SL-2: did the app actually start, and did it draw? ----
  #
  # Two numbers decide whether a window is black for an interesting reason.
  # SWAPS is the honest one: no eglSwapBuffers means the guest never presented a
  # frame, so nothing downstream — sink, compositor, window — can be at fault.
  # And `SDL_main returned` means the guest EXITED, which for this app is the
  # normal no-host outcome, not a crash. Printing them together is what stops a
  # black window being mistaken for a rendering bug.
  if g -q 'phase 4' "$LOG"; then
    echo "  -- SL-2: the app --"
    g -E 'Audio initialized|Video initialized|Desktop mode|Created .* renderer|Initialized player|present\]|guest is (MONO|STEREO)' \
      "$LOG" | sed 's/^\[[0-9]*\/SDL\/APP\] //; s/^ *//; s/^/    /' | awk '!seen[$0]++' || true
    local swaps
    swaps=$(g -cE 'view: \[mono\] frame|eglSwapBuffers' "$LOG" 2>/dev/null || echo 0)
    g -E 'MESSAGEBOX' "$LOG" | sed 's/^\[jni\] /    /' || true
    if g -q 'nativeRunMain returned' "$LOG"; then
      echo "    NOTE: SDL_main RETURNED — the guest exited on its own."
      echo "          With no Steam host on the network that is the app's normal"
      echo "          path, and it exits before drawing anything. A black window"
      echo "          here is the guest not running, not the compositor."
    fi
    local lit
    lit=$(g -oE 'lit=[0-9]+' "$LOG" | g -vE 'lit=0$' | head -1)
    echo "    frames drawn: $(g -oE 'frame [0-9]+' "$LOG" | tail -1 | awk '{print $2+0}')${lit:+, $lit}"
  fi

  # ---- where it stopped, by name ----
  #
  # The stop IS the measurement, so this is the line the whole script exists to
  # surface. Each pattern is a different kind of gap and they are printed in the
  # order they are worth reading: a named miss beats a signal number every time.
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
  # rather than printing nothing, which reads as a clean run. Under --gap the
  # stop is deliberate, so saying "no named stop" there would be a false alarm.
  if [ -z "$found" ]; then
    if [ -n "${KL_GAP_ONLY:-}" ]; then
      echo "    (--gap: stopped before DT_INIT_ARRAY on purpose — nothing was run)"
    elif [ "${rc:-0}" -eq 0 ]; then
      echo "    clean: SL-1 passed with no unimplemented call reached"
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
# incremental build leaves the last good ./build/m_slink in place, so running
# anyway would test the previous code and report it as the new code's result —
# the same trap visionos/run.sh had (CLAUDE.md, "Verify the artifact").
echo "building  : build/m_slink"
BUILDLOG="${LOG%.log}-build.log"
make build/m_slink > "$BUILDLOG" 2>&1
build_rc=$?
g -E 'error|warning:' "$BUILDLOG" | sed 's/^/  /' | head -20
if [ $build_rc -ne 0 ]; then
  echo "!! build failed (see $BUILDLOG) — NOT running the previous binary"
  exit 1
fi

KNOBS="${KL_SLINK_SHELL:+KL_SLINK_SHELL=1 }${KL_GAP_ONLY:+KL_GAP_ONLY=1 }${KL_PERMISSIVE:+KL_PERMISSIVE=1 }${KL_GLFB:+KL_GLFB=$KL_GLFB }${KL_NOFORK:+KL_NOFORK=1 }${KL_ALARM:+KL_ALARM=$KL_ALARM }${KL_TRACE_FS:+KL_TRACE_FS=1 }${KL_TRACE_NET:+KL_TRACE_NET=1 }"
echo "libdir    : $LIBDIR"
echo "knobs     : ${KNOBS:-(none — the default run)}"
echo "log       : $LOG"

# `script -q /dev/null` gives the child a pty, and the redirect sends it to a
# FILE. Both matter and both cost time to rediscover (CLAUDE.md, "Operating the
# M4 loop"): with stdout a plain file stdio goes fully buffered, and a child
# that dies on a signal rather than through kl_fatal_prepare() loses the entire
# buffer — you get a dozen lines and no report, which reads as a much earlier
# failure than actually happened. A pipe straight to head/sed is the other half
# of the same trap: the child forks, and the report arrives out of order.
if [ -n "$VIEW" ]; then
  # No timeout: the run ends when the window is closed. The window IS the
  # output here, so the log is for afterwards rather than for watching.
  echo "            (window open — close it to end the run)"
  script -q /dev/null ./build/m_slink "$LIBDIR" 2>&1 | tee "$LOG"
else
  timeout "$TIMEOUT" script -q /dev/null ./build/m_slink "$LIBDIR"
fi
rc=$?
echo
case $rc in
  0)   echo "exit      : 0" ;;
  124) echo "exit      : TIMEOUT after ${TIMEOUT}s — the guest is blocked on something;" \
            "raise --alarm to let the watchdog report what, or --timeout to wait longer" ;;
  *)   echo "exit      : $rc$([ $rc -gt 128 ] && echo "  (signal $((rc - 128)))")" ;;
esac
summarise
exit $rc
