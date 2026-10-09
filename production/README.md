# OTA production references — metadata, not firmware source

`motion/CURRENT` and `human-presence/CURRENT` point to immutable version directories that contain `RELEASE.json` metadata. **The executable .bin OTA bytes remain in GitHub Releases**, selected by the production server's active release records and streamed through its download proxy.

Do not edit or delete published release assets. Do not overwrite a versioned production directory. For a new firmware release, build and validate an isolated candidate, verify matching flash and OTA SHA-256 digests, preserve source/config and rollback data, then advance `CURRENT` and release registration through controlled promotion.

As of 2026-10-08: both current assets are published with matching metadata checksums, but their source-to-binary build chains are not proven in this repository. See `manifest.json` and per-family `RELEASE.json`.

The manufacturer's local `Desktop/production/.../CURRENT` files are separate and have **not** been independently inspected in this GitHub synchronization.

## Distinguish the Motion release variants

The original `v2.1.0-commissioning-v2` release (`06c55948...`) and later `v2.1.0-commissioning-v2-recommission-20260924` release (`24d89a79...`) have **different application binaries**. The latter is registered as current OTA; the former is reported by Work to match the protected Mac application binary. See [Motion variant metadata](motion/OTA_VARIANTS.json) and [reconciliation status](RECONCILIATION_STATUS.md).

Matching a firmware version string does not prove matching firmware. Do not reinterpret these GitHub `CURRENT` files as the local manufacturing `CURRENT` values.
