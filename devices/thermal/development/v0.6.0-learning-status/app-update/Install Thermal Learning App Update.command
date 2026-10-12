#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
/usr/bin/python3 - <<'INSTALL'
from pathlib import Path
import json, hashlib, shutil, datetime, subprocess
root=Path('/Users/mtamacmini/Developer/GoodShepherdHomeMonitoringSystem')
dst=root/'GoodShepherdHomeMonitoringSystem/ThermalCameraView.swift'
src=Path('ThermalCameraView.swift')
expected=json.loads(Path('source-checkpoint.json').read_text())['ThermalCameraView.swift']
replacement='5ba47a9660c978da293338ef986154fcdf250442acfe7ba32732c7660da89a91'
if hashlib.sha256(src.read_bytes()).hexdigest()!=replacement:
 raise SystemExit('Stopped: the prepared app file changed. Nothing installed.')
current=hashlib.sha256(dst.read_bytes()).hexdigest()
if current==replacement:
 print('The thermal learning app file is already installed.')
elif current!=expected:
 raise SystemExit('Stopped: your app file changed since preparation. Nothing replaced; ask Codex to refresh this update.')
else:
 backup=root.parent/'GoodShepherd-Thermal-Learning-Backups'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
 backup.mkdir(parents=True,exist_ok=False)
 shutil.copy2(dst,backup/dst.name)
 shutil.copy2(src,dst)
 print('Complete app file installed. Previous version saved:',backup)
print('NEXT: In Xcode, select your iPhone and click Run to install the updated app.')
print('Your existing View live button stays available. Room learning appears above it.')
subprocess.run(['open',str(root/'GoodShepherdHomeMonitoringSystem.xcodeproj')],check=True)
INSTALL
