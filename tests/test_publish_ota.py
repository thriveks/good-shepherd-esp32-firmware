import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("publisher", Path(__file__).parents[1] / "tools/publish_ota.py")
publisher = importlib.util.module_from_spec(spec); spec.loader.exec_module(publisher)


class PublisherSafety(unittest.TestCase):
    def test_draft_lookup_uses_authenticated_paginated_releases(self):
        draft = {"tag_name": "test", "draft": True, "id": 1}
        with patch.object(publisher, "command", return_value=json.dumps([[{"tag_name": "other"}], [draft]])) as command:
            self.assertEqual(publisher.find_release("test"), draft)
            self.assertIn("--paginate", command.call_args.args[0])
            self.assertNotIn("/tags/", command.call_args.args[0][2])
        with patch.object(publisher, "command", return_value=json.dumps([[draft, draft]])):
            with self.assertRaises(ValueError): publisher.find_release("test")

    def test_both_families_resume_drafts_and_published_retries_do_not_mutate(self):
        for family in ("motion", "human-presence"):
            with self.subTest(family=family), tempfile.TemporaryDirectory() as temp:
                old_cwd = Path.cwd()
                try:
                    os.chdir(temp)
                    folder = Path("ota-inbox/test"); folder.mkdir(parents=True)
                    version = "esp32-good-shepherd-" + family + "-test"
                    asset = folder / ("good_shepherd_esp32_" + family.replace("-", "_") + ".ino.bin")
                    asset.write_bytes(b"\xe9" + version.encode() + b"\0")
                    r = {"tag": "test", "family": family, "firmwareVersion": version, "sha256": hashlib.sha256(asset.read_bytes()).hexdigest(), "sourceRevision": "a"*40, "runId": 1}
                    request = folder / "request.json"; request.write_text(json.dumps(r))
                    release = {"tag_name": "test", "id": 1, "draft": True, "immutable": False, "assets": []}
                    mutations = []
                    def command(args):
                        if args[:2] == ["gh", "api"]:
                            return json.dumps([[release]]) if "--slurp" in args else json.dumps(release)
                        mutations.append(args)
                        if args[2] == "upload":
                            file = Path(args[4]); release["assets"].append({"name": file.name, "digest": "sha256:" + hashlib.sha256(file.read_bytes()).hexdigest()})
                        elif args[2] == "edit":
                            release.update(draft=False, immutable=True)
                        else: self.fail("Unexpected create/delete/overwrite")
                        return ""
                    with patch.object(publisher, "command", side_effect=command):
                        publisher.main()
                        self.assertEqual(len(mutations), 3)
                        mutations.clear(); publisher.main()
                        self.assertEqual(mutations, [])
                        asset.write_bytes(b"different")
                        with self.assertRaises(ValueError): publisher.main()
                        self.assertEqual(mutations, [])
                finally: os.chdir(old_cwd)


if __name__ == "__main__": unittest.main()
