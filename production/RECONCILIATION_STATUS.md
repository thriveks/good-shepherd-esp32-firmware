# Firmware reconciliation — evidence boundary (2026-10-08)

## Verified independently against GitHub and Render

- The active Motion OTA record points to the **recommission** asset, SHA-256 `24d89a796035ce905aff281de17867bb4caef1750dad2f4a03d09eb04b9e345f`.
- GitHub also has a distinct **original Motion commissioning** release, SHA-256 `06c559484a7b876a88a0c28daa8a353b53dcd3939f089a6a4a2f0ecad915363d`.
- The active Human Presence v2.4.8 OTA record points to SHA-256 `869dac0a8791be3caa66d81d57adfe0e490223eb4c3cc89f5e23ed4d90bb0ff5`.
- GitHub `production/*/CURRENT` describes the active OTA release **only**. It is NOT a claim about, nor a write to, the manufacturer's local `~/Desktop/production/*/CURRENT`.

## Reported by Work, not independently re-ingested here

Work's 2026-10-08 summary reports:

| Finding | Motion | Human Presence |
| --- | --- | --- |
| Protected application binary matches active OTA digest | **No** | Yes |
| Protected source compiles | Yes | Yes |
| Rebuilt executable segments compare with OTA, excluding build metadata | Yes (reported) | Yes (reported) |
| Byte-for-byte reproducibility established | **No** | **No** |

Work reports the protected Motion application digest is `06c55948...`, matching GitHub's **original commissioning** variant. Work also reports its protected Human Presence source differs from the currently checked-in development source. The full report and sanitized source/build-metadata archive were referenced by a local Mac path but were **not attached to this chat**. Their exact file-level findings cannot be independently certified or imported without those file bytes.

## Release-source parity (both families)

**UNVERIFIED.** Published release tags and firmware version strings do not prove the source that produced a given binary. The source on the release tags cannot be assumed canonical. The executable-segment observation by Work is not a byte-exact build proof.

## Controlled completion

1. Import Work's `REPORT.md` and sanitized `github-review-package.tar.gz` for actual inspection. Review archive contents and sanitization before any GitHub source import.
2. Identify the exact local protected source and build configurations for **both** production variants; preserve original and recommissioned Motion builds separately.
3. Use `node tools/classify-firmware-binaries.mjs <application.bin> [...]` to classify **application** binaries by actual digest; a merged flash image is not interchangeable with an OTA application binary.
4. Compare the specific original/recommission Motion source diffs and confirm which artifact the unchanged manufacturing flasher references. Do not redirect it on assumptions.
5. Commit the evidence-backed sanitized source and build metadata under immutable release-specific paths, record source revision and the command/toolchain needed for reproduction, verify file-level hashes. Never overwrite published release binaries, OTA registrations, Mac protected packages or flasher.
6. Only mark `sourceParity` VERIFIED when sufficient build evidence exists. Until byte-exact reproducibility is established, keep `reproducibleBuild` UNVERIFIED and explicitly document what was and was not compared.

**Do not trigger a firmware update, deploy the server, replace the release asset, or change the manufacturing baseline as part of source-documentation reconciliation.**
