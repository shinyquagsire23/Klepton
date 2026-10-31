#!/bin/bash
# Actual Lapiz source build, replacing its embedded And64 with the signed API.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 ports/scotland2/tools/build_lapiz.py
