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
  process.stdout.write(`PASS ${family}: ${pointer} [source parity: ${entry.sourceParity}]\\n`);
}
process.stdout.write("PASS firmware production manifest structure\\n");
