#!/bin/bash
# Build, install and run the probe on a physical Vision Pro.
# Requires: device paired, awake, unlocked, trusted (Xcode > Window > Devices).
#
# Env overrides:
#   KLEPTON_DEVICE=<udid>   skip auto-detection
#   KLEPTON_TEAM=<teamid>   override signing team
#   KLEPTON_BUNDLE_ID=...   override bundle id
set -euo pipefail
cd "$(dirname "$0")"
BUNDLE_ID="${KLEPTON_BUNDLE_ID:-dev.klepton.probe}"

# ---- device discovery (JSON; names contain non-breaking spaces, so never parse columns) ----
find_device() {
  local json; json=$(mktemp)
  xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
  python3 - "$json" <<'PY'
import json, sys
try:
    devs = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    sys.exit(0)
phys = [d for d in devs
        if d.get("hardwareProperties", {}).get("reality") == "physical"
        and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
if not phys:
    print("NONE"); sys.exit(0)
# prefer a connected one
phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
d = phys[0]
name = d.get("deviceProperties", {}).get("name", "?").replace(" ", " ")
print(f'{d["identifier"]}\t{d.get("connectionProperties",{}).get("tunnelState","?")}\t{name}')
PY
  rm -f "$json"
}

if [ -n "${KLEPTON_DEVICE:-}" ]; then
  DEVID="$KLEPTON_DEVICE"; STATE="(override)"; DNAME="(override)"
else
  INFO="$(find_device || true)"
  if [ -z "$INFO" ] || [ "$INFO" = "NONE" ]; then
    echo "!! No physical Vision Pro found."
    echo "   Pair it in Xcode > Window > Devices and Simulators, then retry."
    echo "   Or set KLEPTON_DEVICE=<udid>."
    exit 1
  fi
  DEVID="$(printf '%s' "$INFO" | cut -f1)"
  STATE="$(printf '%s' "$INFO" | cut -f2)"
  DNAME="$(printf '%s' "$INFO" | cut -f3)"
fi
echo "device: $DNAME"
echo "  udid : $DEVID"
echo "  state: $STATE"
if [ "$STATE" != "connected" ] && [ "$STATE" != "(override)" ]; then
  echo
  echo "!! Device is '$STATE', not 'connected'."
  echo "   Put the Vision Pro on (or wake it), unlock it, and keep it awake."
  echo "   Then retry. Continuing anyway in case it reconnects…"
  echo
fi

echo "[1/4] building probe frameworks (device + sim slices)…"
./mkframeworks.sh >/dev/null

echo "[2/4] regenerating project…"
python3 gen_xcodeproj.py

echo "[3/4] building for device…"
# generic/platform, not id= : the build must not depend on the device being awake
set +e
xcodebuild -project KleptonProbe.xcodeproj -scheme KleptonProbe \
  -configuration Debug -destination 'generic/platform=visionOS' \
  -derivedDataPath build/dd-device -allowProvisioningUpdates build 2>&1 \
  | grep -E 'error:|Signing Identity|\*\* BUILD'
set -e

APP="build/dd-device/Build/Products/Debug-xros/KleptonProbe.app"
if [ ! -d "$APP" ]; then
  echo "!! build produced no .app at $APP — see errors above"
  exit 1
fi
echo "    app: $APP"
codesign -dv "$APP" 2>&1 | grep -E 'Authority|TeamIdentifier' || true

echo "[4/4] installing…"
xcrun devicectl device install app --device "$DEVID" "$APP"
echo
echo "Launching — report prints below AND on-screen (Share button to export):"
xcrun devicectl device process launch --device "$DEVID" --console "$BUNDLE_ID"
