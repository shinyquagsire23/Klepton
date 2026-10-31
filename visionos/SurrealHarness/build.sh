#!/bin/bash
# Build a small, ad-hoc signed Simulator app from the production controller source files.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="build/SurrealHarness.app"
mkdir -p "$OUT"
xcrun --sdk xrsimulator swiftc -target arm64-apple-xros26.0-simulator -sdk "$(xcrun --sdk xrsimulator --show-sdk-path)" \
  -parse-as-library -o "$OUT/SurrealHarness" \
  Sources/KleptonControllerInput.swift Sources/KleptonControllerMode.swift \
  Sources/KleptonSurrealInput.swift Sources/KleptonSurrealBackend.swift Sources/KleptonSurrealView.swift \
  Sources/KleptonSurrealAim.swift Sources/KleptonSurrealAimView.swift Sources/KleptonSurrealSDK.swift \
  SurrealHarness/SurrealHarnessApp.swift
cp SurrealHarness/Info.plist "$OUT/Info.plist"
codesign --force --sign - "$OUT"
echo "$PWD/$OUT"
