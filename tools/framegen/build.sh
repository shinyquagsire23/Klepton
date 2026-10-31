#!/bin/bash
# Builds build/framegen: the frame generation measurement (tools/framegen/main.swift)
# with the generator the compositor uses (visionos/Sources/KleptonFrameGen.swift).
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build
swiftc -O -swift-version 5 -target arm64-apple-macos14.0 \
  visionos/Sources/KleptonFrameGen.swift tools/framegen/main.swift \
  -framework Metal -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
  -o build/framegen
echo "built build/framegen"
swiftc -O -swift-version 5 -target arm64-apple-macos14.0 \
  visionos/Sources/KleptonFrameGen.swift tools/framegen/synthetic/main.swift \
  -framework Metal -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
  -o build/framegen_synthetic
echo "built build/framegen_synthetic"
swiftc -O -swift-version 5 -target arm64-apple-macos14.0 \
  visionos/Sources/KleptonFrameGen.swift tools/framegen/mv/main.swift \
  -framework Metal -framework CoreGraphics -framework ImageIO -framework UniformTypeIdentifiers \
  -o build/framegen_mv
echo "built build/framegen_mv"
