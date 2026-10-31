#!/usr/bin/env bash
# Pull the two bounded PCM captures off the headset.
#
# The audio path has a capture at both ends (see kl_audio.h): the guest's own
# callback bytes (KL_AAUDIO_DUMP) and the frames the render callback hands
# CoreAudio (KL_AUDIO_DUMP), each a bounded, self-closing WAV in the app's data
# container. This retrieves them — a capture nobody can read back is not
# evidence.
#
# The data container is the ONLY place `devicectl device copy from` can read,
# and its UUID changes on every reinstall, so the path is named by domain
# rather than spelled out. Both files are overwritten on every launch: pull
# them BEFORE relaunching, or the run you want is gone.
#
#   KLEPTON_BUNDLE_ID=<bundle id> ./visionos/pull_audio.sh [dest-dir]
#                                                 # default dest: build/logs
#
# Then listen. That is the whole point:
#   afplay build/logs/klepton-audio-guest.wav
#   afplay build/logs/klepton-audio-out.wav
# ...and if the guest file is already wrong, the corruption is the guest's (or
# the format we hand it); if only the device file is wrong, it is ours.
set -euo pipefail

BUNDLE_ID="${KLEPTON_BUNDLE_ID:?set KLEPTON_BUNDLE_ID to the installed app's bundle id}"
DEST="${1:-$(cd "$(dirname "$0")/.." && pwd)/build/logs}"
mkdir -p "$DEST"

DEVID="${KLEPTON_DEVICE:-}"
if [ -z "$DEVID" ]; then
  # Same JSON discovery run.sh uses, and for the same reason: device names
  # contain non-breaking spaces, so column parsing picks the wrong field.
  json=$(mktemp)
  xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
  DEVID=$(python3 -c '
import json, sys
try:
    devs = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    sys.exit(0)
phys = [d for d in devs
        if d.get("hardwareProperties", {}).get("reality") == "physical"
        and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
if phys:
    print(phys[0]["identifier"])
' "$json")
  rm -f "$json"
fi
[ -n "$DEVID" ] || { echo "no visionOS device found (set KLEPTON_DEVICE)" >&2; exit 1; }

got=0
for f in klepton-audio-guest.wav klepton-audio-out.wav; do
  if xcrun devicectl device copy from --device "$DEVID" \
       --domain-type appDataContainer --domain-identifier "$BUNDLE_ID" \
       --source "Documents/$f" --destination "$DEST/$f" >/dev/null 2>&1; then
    echo "  $DEST/$f  ($(wc -c < "$DEST/$f" | tr -d ' ') bytes)"
    got=$((got+1))
  else
    # Absent is a finding, not a failure: the capture arms on the first buffer
    # that carries signal, so a missing file means the guest never produced
    # one — which the log's own census will already have said.
    echo "  (no $f in the container — the capture never armed, i.e. no lit audio)"
  fi
done
[ "$got" -gt 0 ] || exit 1
