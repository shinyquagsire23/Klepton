#!/bin/bash
# The incomplete progressive-composition app helper is retired.
# One release builder now owns preflight, selection and package validation.
set -euo pipefail
exec python3 "$(dirname "$0")/tools/build_release.py" "$@"
