#!/usr/bin/env python3
"""Only application bytes and nonsecret provenance enter public releases."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

REPO = "thriveks/good-shepherd-esp32-firmware"


def command(args):
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError("GitHub publication failed: " + " ".join(args[:3]))
    return p.stdout


def api(path):
    return json.loads(command(["gh", "api", "repos/" + REPO + "/" + path]))


def main():
    if api("immutable-releases").get("enabled") is not True:
        raise ValueError("Repository release immutability must be enabled")
    for request in sorted(Path("ota-inbox").glob("*/request.json")):
        r = json.loads(request.read_text())
        family = r["family"]
        if family not in ("motion", "human-presence") or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", r["tag"]) or request.parent.name != r["tag"]:
            raise ValueError("Invalid inbox release identity")
        if not re.fullmatch("esp32-good-shepherd-" + family + r"-[A-Za-z0-9._-]+", r["firmwareVersion"]) or not re.fullmatch(r"[a-f0-9]{64}", r["sha256"]) or not re.fullmatch(r"[a-f0-9]{40}", r["sourceRevision"]):
            raise ValueError("Invalid provenance")
        asset_name = "good_shepherd_esp32_" + family.replace("-", "_") + ".ino.bin"
        asset = request.parent / asset_name
        if asset.is_symlink() or request.is_symlink():
            raise ValueError("Symlink not allowed")
        image = asset.read_bytes()
        if image[:1] != b"\xe9" or len(image) > 0x1E0000 or hashlib.sha256(image).hexdigest() != r["sha256"] or r["firmwareVersion"].encode() + b"\0" not in image:
            raise ValueError("Application digest/header/slot/version mismatch")
        probe = subprocess.run(["gh", "api", f"repos/{REPO}/releases/tags/{r['tag']}"], capture_output=True, text=True)
        if probe.returncode:
            # Distinguish absence from API/auth/transient errors.
            if "HTTP 404" not in probe.stderr:
                raise RuntimeError("Release lookup failed")
            command(["gh", "release", "create", r["tag"], "-R", REPO, "--target", "main", "--draft", "--prerelease", "--title", r["firmwareVersion"], "--notes", "Approved OTA application; staging does not activate production. SHA-256: " + r["sha256"]])
            release = api("releases/tags/" + r["tag"])
        else:
            release = json.loads(probe.stdout)
        if release["draft"]:
            # Resume interrupted draft creation without overwriting any existing asset.
            assets = {a["name"]: a for a in release["assets"]}
            expected_assets = {asset_name, "request.json"}
            if set(assets) - expected_assets:
                raise ValueError("Unexpected draft assets; refusing publication")
            for file in (asset, request):
                expected_hash = hashlib.sha256(file.read_bytes()).hexdigest()
                if file.name in assets:
                    if assets[file.name].get("digest") != "sha256:" + expected_hash:
                        raise ValueError("Draft asset collision")
                else:
                    command(["gh", "release", "upload", r["tag"], str(file), "-R", REPO])
            command(["gh", "release", "edit", r["tag"], "-R", REPO, "--draft=false", "--prerelease", "--latest=false"])
            release = api("releases/tags/" + r["tag"])
        assets = {a["name"]: a for a in release["assets"]}
        if release.get("immutable") is not True or release["draft"] or set(assets) != {asset_name, "request.json"} or any(assets[f.name].get("digest") != "sha256:" + hashlib.sha256(f.read_bytes()).hexdigest() for f in (asset, request)):
            raise ValueError("Published immutable release identity mismatch")
        print("VERIFIED IMMUTABLE:", r["tag"], r["sha256"])


if __name__ == "__main__":
    main()
