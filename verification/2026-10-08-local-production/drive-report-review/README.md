# 2026-10-08 protected production audit — Drive report review

## Evidentiary scope

This subdirectory records **findings extracted from the Mac-side Work report** titled `Good Shepherd Firmware Verification REPORT.md`, retrieved from the connected Google Drive on 2026-10-08. The report described local files inspected by Work, not files mounted here. The associated sanitized `github-review-package.tar.gz` was **not found in Drive search during this pass**, so production source has **not** been imported or independently rehashed here.

These files are report-derived metadata only and must not be represented as signed build provenance, a reproducible build, or proof that the protected manufacturing folders are synchronized to the active OTA asset.

## Findings

| Check | Motion | Human Presence |
| --- | --- | --- |
| Protected application SHA-256 | `06c559484a7b876a88a0c28daa8a353b53dcd3939f089a6a4a2f0ecad915363d` | `869dac0a8791be3caa66d81d57adfe0e490223eb4c3cc89f5e23ed4d90bb0ff5` |
| Active OTA application SHA-256 | `24d89a796035ce905aff281de17867bb4caef1750dad2f4a03d09eb04b9e345f` | `869dac0a8791be3caa66d81d57adfe0e490223eb4c3cc89f5e23ed4d90bb0ff5` |
| Local package matches active OTA | **NO** | **YES** |
| Protected source compiles in isolated build | **YES (Work reported)** | **YES (Work reported)** |
| Metadata-excluded executable segments match OTA | **YES (Work reported)** | **YES (Work reported)** |
| Exact reproducible build | **NOT ESTABLISHED** | **NOT ESTABLISHED** |

Motion's local protected firmware is the application published in the **original** commissioning release, not the **later recommissioned** OTA release; the versions have the same firmware string. The report says the Motion protected source is identical to the current GitHub Motion workbench, while the protected Human Presence source differs from the existing development workbench in device-name handling. Do not overwrite either development workbench.

## Evidence files here

- `ARTIFACT-INVENTORY.json`: Work-reported protected source and artifact digests, local/OTA/rebuild relationship, and provenance caveats.
- `ISOLATED-BUILD-ENVIRONMENT.json`: Work-reported toolchain, partitions and rebuild limitations.
- Actual sanitized source files, `verification.json`, and `SOURCE-PROVENANCE.json` must come from the **unavailable** review-package tarball. Do not invent them from the report.

## Change control

Preserve all shipping release assets and the current OTA database records. Preserve existing `production/*/CURRENT` GitHub pointers (OTA references) and local manufacturer pointers (separate responsibility). No flashing, OTA scheduling, production release promotion, or source-to-binary certification is authorized by this evidence alone.
