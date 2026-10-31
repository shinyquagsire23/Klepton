#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 ports/scotland2/tools/build_songdownloader.py
