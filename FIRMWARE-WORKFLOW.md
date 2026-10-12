# Good Shepherd Firmware Workflow

## Source of truth

Never determine current firmware from the newest-looking development
folder, checkpoint, archive, Arduino scratch copy, or Codex copy.

The authoritative current releases are:

### Motion

`production/motion/CURRENT`

### Human Presence

`production/human-presence/CURRENT`

`CURRENT` points to an immutable versioned production release.

Each production release contains:

- protected source
- VERSION
- firmware.bin
- bootloader.bin
- partitions.bin
- boot_app0.bin
- firmware.merged.bin
- SHA256SUMS
- MANIFEST.md

## Normal OTA update

Use the Good Shepherd Command Center **Update firmware** workflow.

The version displayed in Command Center must match the intended
production VERSION before a rollout is considered current.

`firmware.bin` is the application image used for OTA packaging.

## USB / recovery / manufacturing flash

Double-click:

`tools/Flash Good Shepherd Sensor.command`

Then:

1. Choose Motion or Human Presence.
2. Plug in the ESP32.
3. Confirm the detected USB port.
4. Verify the displayed production firmware version.
5. Type `FLASH`.
6. Wait for FLASH COMPLETE.

The flash helper verifies release SHA-256 checksums before writing.

It does not issue an erase-flash operation.

## Development rule

Never modify a production release in place.

For a new firmware version:

1. Create a new development workbench.
2. Preserve the current production release.
3. Build and test the candidate.
4. Verify the firmware's SOFTWARE_VERSION.
5. Verify the final compiled binary contains that exact version.
6. Create a new immutable production release directory.
7. Generate SHA256SUMS and MANIFEST.md.
8. Test USB flashing on a test sensor.
9. Test commissioning/network/MQTT behavior.
10. Promote by updating CURRENT only after validation.
11. Update OTA distribution only after promotion.

## Rollback

Previous production release directories remain untouched.

Rollback means repointing CURRENT to the previously approved release
and deliberately redeploying that known-good firmware.

Do not delete old production releases during promotion.

## Firmware naming

From 2026-10-12 onward, do not include the word `test` in new firmware
SOFTWARE_VERSION strings, VERSION files, or customer-facing firmware names.
Use a clean version with an optional descriptive suffix, for example
`esp32-good-shepherd-thermal-v0.6.0-learning-status`.

Keep development, validation, approval and promotion status in the workbench
location, build metadata and evidence reports. Clean naming does not imply
production approval or weaken any build, spare-board or promotion checks.
Preserve historical release names, compiled artifacts and rollback folders.
