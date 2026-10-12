#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
/bin/bash "Flash Thermal.command"
echo
echo 'Starting the five-minute automatic learning check. Keep the camera fixed in the same view as the previous test.'
echo 'Capture begins after the normal startup window. No manual calibration is needed.'
exec /usr/bin/python3 -u tools/record_local.py
