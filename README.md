# Good Shepherd ESP32 Firmware

## Current production OTA releases (verified 2026-10-08)

These are the **actual active OTA releases** in the Good Shepherd production server's firmware-release records, and the SHA-256 digests match the GitHub release assets.

| Device family | Production firmware version | GitHub release | Firmware binary SHA-256 |
| --- | --- | --- | --- |
| Motion (PIR) | `esp32-good-shepherd-motion-v2.1.0-commissioning-v2` | [v2.1.0-commissioning-v2-recommission-20260924](https://github.com/thriveks/good-shepherd-esp32-firmware/releases/tag/v2.1.0-commissioning-v2-recommission-20260924) | `24d89a796035ce905aff281de17867bb4caef1750dad2f4a03d09eb04b9e345f` |
| Human presence (LD2410) | `esp32-good-shepherd-human-presence-v2.4.8-recommission-v1` | [v2.4.8-recommission-20260924](https://github.com/thriveks/good-shepherd-esp32-firmware/releases/tag/v2.4.8-recommission-20260924) | `869dac0a8791be3caa66d81d57adfe0e490223eb4c3cc89f5e23ed4d90bb0ff5` |

The OTA service publishes these versions through:
- Motion: `https://good-shepherd-server-j06f.onrender.com/firmware/latest` (firmware-family-aware with `nodeId`).
- Human presence: `https://good-shepherd-server-j06f.onrender.com/firmware/human-presence/latest`.

The OTA binaries are release assets, **not** the default-branch Arduino sketch.

## Source-code verification status: NOT YET SYNCHRONIZED

**Do not build or debug current production firmware from `good_shepherd_esp32_motion/good_shepherd_esp32_motion.ino`.** This checked-in sketch identifies itself as `esp32-good-shepherd-v1.9.5-true-lite-heartbeat` and is an older historical implementation. The sketch associated with the current motion release tag also identifies as v1.9.5; this does **not** establish that it produced the v2.1.0 binary.

The human-presence v2.4.8 production source is likewise **not present** on this repository's default branch. Neither active firmware asset has an independently verified, reproducible source-to-binary build recorded here.

Authoritative local production pointers previously established for the manufacturer are:
- `Desktop/production/motion/CURRENT`
- `Desktop/production/human-presence/CURRENT`

These local files must be inspected, secret-scanned, and checked against each release before claiming source parity or publishing them to Git.

## Security and change control

**This repository is currently public.** Do **not** upload production firmware source, Wi-Fi credentials, MQTT credentials, webhook/service secrets, or factory configuration here while public. The existing historical source should be treated as potentially containing exposed credentials; rotating any exposed credentials requires a coordinated firmware migration.

Make the repository private using GitHub repository administration **before** importing production sources. Preserve the released binaries, their tags and digests for rollback. Do not overwrite immutable releases or push OTA updates merely to repair Git source bookkeeping.

Last verified from the production database and GitHub releases: 2026-10-08. This document is release metadata, **not** source parity certification.

## Canonical firmware tree (GitHub main)

- `devices/motion/development/`: editable PIR workbenches; **not** automatically production source.
- `devices/human-presence/development/`: editable LD2410 workbenches; **not** automatically production source.
- `devices/{motion,human-presence}/testing/`: isolated firmware candidate evaluation (do not publish directly to OTA).
- `production/{motion,human-presence}/CURRENT`: canonical GitHub references for today's OTA versions.
- `production/{motion,human-presence}/<version>/RELEASE.json`: GitHub release asset tag, filename, checksum and the **unverified** source-parity state.
- `production/manifest.json`: production release registry checked by `node tools/verify-production-manifests.mjs`.
- `releases/`: documentation and metadata; compiled OTA binaries continue to live in GitHub Releases.
- `archive/`: historical rollback references. The root `good_shepherd_esp32_motion/` sketch remains a v1.9.5 legacy copy; never treat it as live production source.

**Release rule:** Development → isolated testing → record test evidence → archive exact source, build settings and flash images → verify binary SHA-256 matches the immutable GitHub Release asset → update production manifest/CURRENT after controlled promotion. Updating GitHub source files or manifests alone does **not** change device OTA versions. The production server's active release records control OTA selection.

**Outstanding:** inspect the protected manufacturer folders `Desktop/production/motion/CURRENT` and `Desktop/production/human-presence/CURRENT`, confirm source/build provenance, and match local flash artifacts to the OTA asset checksums before marking source parity VERIFIED. Do not relabel workbench source as binary provenance.
