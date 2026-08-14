#!/usr/bin/env bash
set -euo pipefail

capture_output="${1:-build/openchat-reference.png}"
capture_width="${2:-860}"
capture_height="${3:-680}"

QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    ./build/OpenChat \
    --capture "${capture_output}" \
    --capture-delay 250 \
    --width "${capture_width}" \
    --height "${capture_height}"
