#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
exec /usr/bin/python3 -u tools/record_local.py --calibrate-empty
