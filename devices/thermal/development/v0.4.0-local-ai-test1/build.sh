#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
TASK_ROOT="$PWD"
mkdir -p build artifacts evidence tool-state/user tool-state/downloads
python3 - <<'PY'
from pathlib import Path
import json, os
r=Path.cwd()
Path('arduino-cli.yaml').write_text('directories:\n  data: '+json.dumps(str(Path.home()/'Library/Arduino15'))+'\n  downloads: '+json.dumps(str(r/'tool-state/downloads'))+'\n  user: '+json.dumps(str(r/'tool-state/user'))+'\n')
PY
if ! arduino-cli version | /usr/bin/grep -q 'Version: 1.5.1 '; then
  echo 'Pinned arduino-cli 1.5.1 required'; exit 1
fi
arduino-cli --config-file arduino-cli.yaml core list > evidence/installed-cores.txt
/usr/bin/grep -Eq '^esp32:esp32[[:space:]]+3.3.11[[:space:]]' evidence/installed-cores.txt
(shasum -a 256 -c SOURCE_SHA256SUMS > evidence/source-integrity.txt)
export SOURCE_DATE_EPOCH=1791763200
arduino-cli --config-file arduino-cli.yaml compile --clean \
  --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashMode=dio,FlashFreq=80,FlashSize=4M' \
  --libraries "$TASK_ROOT/vendor" --build-path "$TASK_ROOT/build" \
  --output-dir "$TASK_ROOT/artifacts" "$TASK_ROOT/source/good_shepherd_thermal_test" \
  2>&1 | tee evidence/build.log

(cd artifacts && shasum -a 256 *.bin *.elf *.map > SHA256SUMS)
