import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const load = (p) => JSON.parse(readFileSync(path.join(root, p), "utf8"));
const registry = load("production/manifest.json");
assert.equal(registry.schemaVersion, 1);
const families = ["motion", "human-presence"];
for (const family of families) {
  const pointer = readFileSync(path.join(root, "production", family, "CURRENT"), "utf8").trim();
  const relative = `production/${family}/${pointer}/RELEASE.json`;
  assert.ok(existsSync(path.join(root, relative)), `Missing ${relative}`);
  assert.equal(registry.families[family], relative);
  const entry = load(relative);
  assert.equal(entry.schemaVersion, 1);
  assert.equal(entry.deviceFamily, family);
  assert.equal(entry.firmwareVersion, pointer);
  assert.match(entry.assetSha256, /^[a-f0-9]{64}$/);
  assert.match(entry.releaseTag, /^v[0-9][A-Za-z0-9._-]*$/);
  assert.equal(entry.binaryStorage, "github-release-asset");
  assert.ok(["UNVERIFIED", "VERIFIED"].includes(entry.sourceParity));
  if (entry.sourceParity === "VERIFIED") {
    assert.ok(entry.sourceRevision, "VERIFIED source parity requires immutable sourceRevision");
    assert.equal(entry.reproducibleBuild, "VERIFIED", "VERIFIED source parity requires reproducible build");
  }
  const expectedAsset = family === "motion" ? "good_shepherd_esp32_motion.ino.bin" : "good_shepherd_esp32_human_presence.ino.bin";
  const expectedRoute = family === "motion" ? "/firmware/download/" : "/firmware/human-presence/download/";
  assert.equal(entry.assetName, expectedAsset);
  assert.equal(entry.githubReleaseUrl, `https://github.com/thriveks/good-shepherd-esp32-firmware/releases/tag/${entry.releaseTag}`);
  assert.equal(entry.otaDownloadUrl, `https://good-shepherd-server-j06f.onrender.com${expectedRoute}${entry.releaseTag}/${entry.assetName}`);
  process.stdout.write(`PASS ${family}: ${pointer} [source parity: ${entry.sourceParity}]\n`);
}
process.stdout.write("PASS firmware production manifest structure\n");

// Guard against conflating the two same-version Motion release variants.
const variants = load("production/motion/OTA_VARIANTS.json");
const currentMotion = load(registry.families.motion);
assert.equal(variants.schemaVersion, 1);
assert.equal(variants.deviceFamily, "motion");
assert.equal(variants.firmwareVersion, currentMotion.firmwareVersion);
assert.equal(variants.activeOtaReleaseTag, currentMotion.releaseTag);
assert.equal(variants.provenanceStatus, "UNRESOLVED");
assert.equal(variants.publishedAssets.length, 2);
assert.deepEqual(variants.publishedAssets.map((v) => v.role).sort(), ["current-ota-recommission", "original-commissioning"]);
const knownDigests = new Set();
for (const variant of variants.publishedAssets) {
  assert.match(variant.sha256, /^[a-f0-9]{64}$/);
  assert.ok(Number.isInteger(variant.sizeBytes) && variant.sizeBytes > 0);
  assert.match(variant.releaseTag, /^v[0-9][A-Za-z0-9._-]*$/);
  assert.equal(variant.assetName, currentMotion.assetName);
  assert.equal(variant.releaseUrl, `https://github.com/thriveks/good-shepherd-esp32-firmware/releases/tag/${variant.releaseTag}`);
  assert.ok(!knownDigests.has(variant.sha256), "Release variants must remain distinguishable");
  knownDigests.add(variant.sha256);
}
const activeVariant = variants.publishedAssets.find((v) => v.role === "current-ota-recommission");
const originalVariant = variants.publishedAssets.find((v) => v.role === "original-commissioning");
assert.equal(activeVariant.releaseTag, currentMotion.releaseTag);
assert.equal(activeVariant.sha256, currentMotion.assetSha256);
assert.equal(activeVariant.sizeBytes, 1350432);
assert.equal(originalVariant.releaseTag, "v2.1.0-commissioning-v2");
assert.equal(originalVariant.sha256, "06c559484a7b876a88a0c28daa8a353b53dcd3939f089a6a4a2f0ecad915363d");
assert.equal(originalVariant.sizeBytes, 1350576);
assert.equal(variants.localProtectedApplicationObservation.sha256ReportedByWork, originalVariant.sha256);
assert.equal(variants.localProtectedApplicationObservation.independentlyReverifiedFromLocalFile, false);
assert.equal(variants.localProtectedApplicationObservation.differsFromActiveOta, true);
process.stdout.write("PASS distinct Motion original/recommission variants and unresolved local observation\n");
