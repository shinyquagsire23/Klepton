#!/bin/bash
# Launch the installed app of a Linux target on the headset, without building.
#
#   KL_LX_ARGS="-vr -vulkan +map startup" ./lx_launch.sh [seconds]
#
# run.sh device builds, installs and stages before it launches, which is minutes
# when the only thing that changed is an argument. This is its last step alone:
# every KL_* variable in the environment is forwarded to the app, the app's
# output is streamed into $KL_LOG_OUT (default /tmp/klepton-device.log), and the
# script returns when the app exits or after [seconds] (default 120) — the app
# keeps running in the second case.
set -euo pipefail
cd "$(dirname "$0")"
eval "$(python3 targets.py "${KLEPTON_TARGET:-alyx}")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-$KLT_BUNDLE}"
DEVID="${KLEPTON_DEVICE:-$(python3 -c 'import stage_sync; print(stage_sync.find_device() or "")')}"
[ -n "$DEVID" ] || { echo "!! no Vision Pro — set KLEPTON_DEVICE"; exit 1; }
LOG="${KL_LOG_OUT:-/tmp/klepton-device.log}"
ENVJSON='{"KL_AUTOBOOT":"1","KL_LOG_FILE":"0"'
for K in $(env | sed -n 's/^\(KL_[A-Za-z0-9_]*\)=.*/\1/p' | sort); do
  case "$K" in KL_LOG_OUT|KL_LOG_FILE|KL_AUTOBOOT) continue ;; esac
  eval "V=\${$K:-}"
  [ -z "$V" ] || ENVJSON="$ENVJSON,\"$K\":\"$V\""
done
ENVJSON="$ENVJSON}"
: > "$LOG"
xcrun devicectl device process launch --device "$DEVID" --console --terminate-existing \
  --environment-variables "$ENVJSON" "$BUNDLE_ID" >> "$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 "${1:-120}"); do
  sleep 1
  kill -0 $PID 2>/dev/null || { echo "app exited — log: $LOG"; exit 0; }
done
echo "app still running — log: $LOG"
