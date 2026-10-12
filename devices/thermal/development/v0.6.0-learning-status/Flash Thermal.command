#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
trap 'echo; echo "Flash stopped. See the message above; no further action was attempted."' ERR
printf '\nGOOD SHEPHERD THERMAL FIRMWARE — v0.6.0-learning-status\n'
echo 'Engineering candidate: use only the spare thermal ESP32; disconnect other USB serial devices.'
echo 'Human/posture/fall classifications remain unknown. This does not promote production firmware.'

CLI="$(command -v arduino-cli || true)"
if [[ -z "$CLI" && -x /opt/homebrew/bin/arduino-cli ]]; then CLI=/opt/homebrew/bin/arduino-cli; fi
if [[ -z "$CLI" && -x /usr/local/bin/arduino-cli ]]; then CLI=/usr/local/bin/arduino-cli; fi
if [[ -z "$CLI" ]]; then echo 'Arduino CLI was not found. Nothing flashed.'; exit 1; fi

shopt -s nullglob
TEST_PORTS=(/dev/cu.usbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART* /dev/cu.wchusbserial* /dev/cu.CP210*)
if [[ ${#TEST_PORTS[@]} -eq 0 ]]; then
  echo 'No USB serial board found. Connect the spare ESP32 with a USB data cable, then open this command again.'
  exit 1
fi
if [[ ${#TEST_PORTS[@]} -ne 1 ]]; then
  echo 'More than one USB serial port found. Disconnect the other devices, then open this command again.'
  printf '  %s\n' "${TEST_PORTS[@]}"
  exit 1
fi
TEST_PORT="${TEST_PORTS[0]}"
echo "Board port: $TEST_PORT"
if /usr/sbin/lsof "$TEST_PORT" >/dev/null 2>&1; then
  echo 'The USB port is busy. Close the thermal viewer or serial monitor, then reopen this file.'
  exit 1
fi
echo 'Checking the compiled thermal firmware files...'
/usr/bin/python3 - <<'VERIFY'
import hashlib, json
from pathlib import Path
r = Path.cwd()
report = json.loads((r/'evidence/build-validation.json').read_text())
if report.get('twoCleanBuildsIdentical') is not True:
    raise SystemExit('Reproducible build verification is missing. Nothing flashed.')
for name, expected in report['artifacts'].items():
    if hashlib.sha256((r/'artifacts'/name).read_bytes()).hexdigest() != expected:
        raise SystemExit('Artifact does not match the validated build: '+name+'. Nothing flashed.')
if b'esp32-good-shepherd-thermal-v0.6.0-learning-status\0' not in (r/'artifacts/good_shepherd_thermal.ino.bin').read_bytes():
    raise SystemExit('Wrong firmware version. Nothing flashed.')
print('Validated v0.6.0 learning-status artifacts match.')
VERIFY
(cd artifacts && shasum -a 256 -c SHA256SUMS)
mkdir -p evidence
FLASH_LOG="evidence/usb-flash-$(date +%Y%m%d-%H%M%S).log"
echo 'Uploading v0.6.0-learning-status to the spare thermal ESP32...'
"$CLI" --config-file arduino-cli.yaml upload \
 --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashMode=dio,FlashFreq=80,FlashSize=4M' \
 --input-dir artifacts --port "$TEST_PORT" source/good_shepherd_thermal \
  2>&1 | tee "$FLASH_LOG"
echo
echo 'FLASH COMPLETE'
echo "Upload log saved: $FLASH_LOG"
echo 'The thermal firmware is now running. Serial diagnostics use 115200 baud.'
echo 'Next: open Record Thermal Diagnostics.command. Learning starts automatically; allow about 40 seconds after capture starts to warm up.'
