# Thermal learning-status v0.6.0

Derived from physically verified v0.5.1. Learning algorithm and on-demand heatmap
policy are unchanged. Adds a bounded learning object to the existing once/minute
thermal_summary message. No new image uploads; no region/person/fall decisions.
Firmware status is shown via authenticated, ownership-checked status endpoints.
App polls status every 15 seconds while its card is visible and app is active.
Server marks summaries stale after 150 seconds and expires them after 180.

Double-click Flash and Record Thermal.command on the spare thermal ESP32 only.
This runs the checked flash helper followed by the five-minute recorder.
The v0.5.1 folder remains available as the USB rollback checkpoint.
After the app replacement is installed and run on iPhone, Room learning should
progress to Ready. View live and Stop live retain the existing behavior.

Build pins are in build-lock.json; clean artifacts are checked by build-validation.
Source is in source/good_shepherd_thermal; host checks are in verification.
Pinned upstream dependencies are in .dependencies.
SOURCE_SHA256SUMS excludes PrivateConfig.h. Never publish credentials or binaries.
Not promoted. Physical MQTT, app status, live image and Stop live need verification.

## Install the iPhone display

Double-click Install Thermal App Update.command. It checks the original file,
preserves a rollback copy, replaces the complete ThermalCameraView.swift file,
and opens Xcode. Select your iPhone and click Run to install the app.
No partial edits or TestFlight upload are required.
The separate replacement passed swiftc parsing and iPhone SDK type-checking;
full project build here was blocked by local simulator-runtime availability.
The real iPhone run and status/image checks remain necessary.

Firmware names follow FIRMWARE-WORKFLOW.md: no word "test" in displayed version names. Validation and promotion status stay in evidence/metadata. Historical rollback folders are immutable. This is a naming-only rebuild; sensing, learning, networking and app behavior match the prior v0.6.0 candidate.

Earlier development snapshots are preserved under ../../archive, with original-path mappings in MANIFEST.json. Use verification/run.sh for the host checks.
