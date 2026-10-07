from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "verify_release_tag", Path(__file__).resolve().parents[2] / "tools/verify_release_tag.py"
)
assert SPEC and SPEC.loader
module = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(module)


class ReleaseTagTests(unittest.TestCase):
    def test_lightweight_and_annotated_tags_match_exact_commit(self):
        commit, tag = "a" * 40, "b" * 40
        for objects in ([{"type": "commit", "sha": commit}],
                        [{"type": "tag", "sha": tag}, {"type": "commit", "sha": commit}]):
            with self.subTest(objects=objects), patch.object(module, "github_object", side_effect=[{"object": x} for x in objects]):
                self.assertEqual(module.verify("Robotweax/srt", "v0.2.7", commit),
                                 {"tag": "v0.2.7", "object": objects[0]["sha"], "commit": commit})

    def test_changed_commit_or_tag_object_fails_before_upload(self):
        for obj, expected_object in (({"type": "commit", "sha": "b" * 40}, None),
                                     ({"type": "tag", "sha": "b" * 40}, "c" * 40)):
            with self.subTest(obj=obj), patch.object(module, "github_object", return_value={"object": obj}):
                with self.assertRaises(ValueError):
                    module.verify("Robotweax/srt", "v0.2.7", "a" * 40, expected_object)

    def test_missing_or_invalid_tag_fails_closed(self):
        with patch.object(module, "github_object", side_effect=RuntimeError("not found")):
            with self.assertRaises(RuntimeError):
                module.verify("Robotweax/srt", "v0.2.7", "a" * 40)
        with patch.object(module, "github_object", return_value={"object": {"type": "tree", "sha": "a" * 40}}):
            with self.assertRaises(ValueError):
                module.verify("Robotweax/srt", "v0.2.7", "a" * 40)

    def test_workflow_checks_before_creation_and_upload(self):
        workflow = (Path(__file__).resolve().parents[2] / ".github/workflows/windows-sdk.yml").read_text()
        step = workflow.split("- name: Attach both installers to a draft only", 1)[1]
        first = step.index("verify_release_tag.py")
        create = step.index("gh release create")
        second = step.index("verify_release_tag.py", first + 1)
        upload = step.index("gh release upload")
        self.assertLess(first, create)
        self.assertLess(create, second)
        self.assertLess(second, upload)
        self.assertIn('--object "$tag_object"', step)
