#!/usr/bin/env node
// Read-only release-asset digest classifier. Never modifies input files.
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const load = (relative) => JSON.parse(readFileSync(path.join(root, relative), "utf8"));
const active = load("production/manifest.json");
const variants = load("production/motion/OTA_VARIANTS.json");
const presence = load(active.families["human-presence"]);
const known = [
  ...variants.publishedAssets.map((v) => ({
    family: "motion", variant: v.role, releaseTag: v.releaseTag,
    sha256: v.sha256, sizeBytes: v.sizeBytes,
    currentOta: v.releaseTag === variants.activeOtaReleaseTag
  })),
  {
    family: "human-presence", variant: "current-ota-recommission",
    releaseTag: presence.releaseTag, sha256: presence.assetSha256,
    currentOta: true, sizeBytes: 1352752
  }
];
const inputs = process.argv.slice(2);
if (!inputs.length || inputs.includes("--help") || inputs.includes("-h")) {
  console.log("Usage: node tools/classify-firmware-binaries.mjs <application.bin> [additional-application.bin ...]");
  console.log("Compares SHA-256 of the supplied file(s) to published OTA application binaries; no changes are made.");
  process.exitCode = inputs.length ? 0 : 2;
} else {
  let failed = false;
  for (const filename of inputs) {
    try {
      const bytes = readFileSync(filename);
      const digest = createHash("sha256").update(bytes).digest("hex");
      const matches = known.filter((entry) => entry.sha256 === digest && entry.sizeBytes === bytes.length);
      if (!matches.length) failed = true;
      console.log(JSON.stringify({
        path: filename,
        sizeBytes: bytes.length,
        sha256: digest,
        classification: matches.length ? matches : "NO_KNOWN_OTA_APPLICATION_ASSET_MATCH",
        warning: "Hash match identifies the published binary only; it does not prove source-to-binary reproducibility. A merged flash image will not match an application-only asset."
      }, null, 2));
    } catch (error) {
      failed = true;
      console.error(JSON.stringify({ path: filename, error: error.message }));
    }
  }
  if (failed) process.exitCode = 1;
}
