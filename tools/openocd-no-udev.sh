#!/usr/bin/env bash

# Run OpenOCD with temporary root privileges when no udev rule may be installed.
# The wrapper does not change udev rules, groups, or any persistent system state.
set -euo pipefail

if ! command -v sudo >/dev/null 2>&1; then
    printf '%s\n' 'error: sudo is required to access the USB HID device without a udev rule' >&2
    exit 127
fi

exec sudo -- /usr/bin/openocd "$@"
