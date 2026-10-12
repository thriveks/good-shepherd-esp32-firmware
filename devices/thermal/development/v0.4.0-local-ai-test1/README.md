# Good Shepherd thermal local AI foundation — v0.4.0-local-ai-test1

**Engineering candidate only. Built and host-tested; NOT flashed, deployed, or promoted.**

Derived from the exact installed v0.3.0-efficiency-test1 source and pinned vendor libraries. Current production pointers, OTA firmware, previous candidates, app and server deployment are unchanged. `evidence/parent-checkpoint.json` preserves the parent. The user's installed version was independently observed through single-node MQTT receipt; the copied aggregate evidence contains no raw images or credentials.

## What changed

- Continuous local complete-frame acquisition, independent of cloud registration/view demand. MLX90640 chess/18-bit at 8 Hz subpage refresh, 400 kHz I2C; scheduler targets at most ~4 complete frames/s. Actual throughput is **unverified on hardware**. BLE/setup still takes priority, and existing reconnect operations can cause gaps; no hard real-time guarantee.
- A fixed-memory native 32×24 background/connected-region processor. Its object size is recorded by the host tests (~8.7 KB). No heap allocation, frame queue, cloud AI upload, trained model or safety alert is introduced.
- Explicit empty-scene calibration: 32 consecutive stable frames after the serial command `THERMAL CALIBRATE EMPTY`. Until then, only acquisition/temperature diagnostics are valid and foreground detection stays uncalibrated. A reboot or >30 second capture pause invalidates the background. Recalibrate after moving the camera or changing the room arrangement; camera motion is not automatically detected.
- Warm foreground threshold 2°C above background, four-connected regions of >=4 native pixels, maximum 8 stored regions. Multiple/overflow regions or scene-wide change are ambiguous. Conservative centroid/area association resets dwell on gaps, invalid frames, displacement, activity or ambiguity. Foreground is excluded from background adaptation, preserving stationary warm regions.
- `stationaryCandidate` after 60 seconds is a research feature, **not human immobility or a fall**. Heaters/objects may create regions; cold/low-contrast/occluded people may be missed. Person, posture and fall remain `unknown`; body temperature remains `unavailable`.
- Serial diagnostics once/second: boot ID, capture sequence/start/end, read/feature durations and maxima, errors/gaps, calibration, regions/bounds/centroid, dwell and temperature aggregates. Raw pixels are never logged by the provided recorder.
- Existing compact image schema, boot-bound 15-second view lease and QoS0/drop policy preserved. Images have a separate >=1 second send interval; lease renewals cannot trigger bursts. Existing minute summaries remain; continuous capture avoids the parent's idle scheduling/summary boundary problem. Network code, auth, BLE commissioning and OTA-disabled test behavior are preserved.

## Reproduce and inspect

Run `tests/run.sh` for host tests with ASan/UBSan and 10,000 stress frames. Run `build.sh` with Arduino CLI 1.5.1, core esp32:esp32@3.3.11 and the local private configuration. Exact libraries/FQBN are in `build-lock.json`; `SOURCE_SHA256SUMS` checks the source/vendor input. PrivateConfig.h is local-only and deliberately excluded. Build artifacts and their manifest are in `artifacts/`. Two clean builds must match; details in `evidence/build-validation.json`.

`research/RESEARCH-AND-MODEL-GATES.md` records primary references, pinned revisions, licenses and remaining dataset/model evaluation. The manufacturer's example and notices are retained for reference, not executed.

## Controlled physical test — still pending

Do not promote this candidate or use it for safety monitoring. This turn did not run a flash helper. A deliberate future USB test uses only the spare thermal ESP32, after recording the v0.3.0 rollback artifact. No existing fleet sensor should be connected or targeted. Keep the installed v0.3.0 one-click helper/artifacts as rollback; restoring v0.3.0 requires an intentional user USB action and must retain commissioning settings.

After a controlled candidate test flash:

1. Mount the D110 securely with the whole intended area visible, confirm orientation/height/angle, let temperatures settle, and leave the scene empty. Close USB serial viewers; cloud app heatmap can remain open.
2. Double-click **Calibrate Empty Scene and Record.command**, then press Return once the scene is empty. Calibration is sent only after candidate diagnostics arrive. Keep the scene empty until `calibrated=True`. It records five minutes of aggregate local diagnostics.
3. Enter/exit, walk across the scene, sit/stand, remain still for >60 seconds, then leave. Repeat with two people and a stationary warm object. Validate features against observations; semantic outputs must stay unknown. Do not stage an actual fall.
4. Test app and Command Center heatmaps, viewing stop/lease expiry and summary cadence. Observe sustained capture rate/read time/errors/heap behavior. In a separate controlled test, interrupt Wi-Fi and exercise BLE: capture must recover, continuity/dwell must reset, and no offline frame backlog may be sent. Recalibrate after long interruptions.
5. **Record Thermal AI Test.command** records without sending commands. Files go to `evidence/physical-test-*.jsonl`; no raw frames or Wi-Fi credentials are saved. A busy port requires closing the other viewer, not killing an unknown process.

Physical results, ESP32 timing/memory under load, mounting calibration and labeled person/posture/fall/immobility accuracy are required before any promotion. There is deliberately no production release/CURRENT/OTA update in this candidate.

## Public source review copy

Private configuration, compiled credential-bearing artifacts, parent-private manifests and physical node receipt logs stay in the local Mac candidate. Only aggregate build/host validation is included here. Copy `PrivateConfig.example.h` to local ignored `PrivateConfig.h` and supply authorized test credentials before building. The artifact digests document the local private-config build; independent credentials produce different binaries. Reproducibility was verified at the same path with the same inputs/toolchain, not across arbitrary paths/configuration.
