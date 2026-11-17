"""Tests of release_notes.py, on a git repository made up here.

Run with: python3 -m unittest discover -s tools/release
"""

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

import release_notes as rn


class MadeUpRepository:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.run("init", "-q", "-b", "main")
        self.run("config", "user.name", "Test")
        self.run("config", "user.email", "test@example.invalid")
        self.run("config", "commit.gpgsign", "false")
        self.run("config", "tag.gpgsign", "false")
        self.count = 0

    def run(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.root), *args], check=True,
                              capture_output=True, text=True).stdout

    def commit(self, message: str) -> None:
        self.count += 1
        (self.root / "file.txt").write_text(str(self.count))
        self.run("add", "file.txt")
        self.run("commit", "-q", "-m", message)

    def tag(self, name: str) -> None:
        self.run("tag", name)


class ReleaseNotesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = MadeUpRepository(Path(self.temp.name))

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_first_release_has_no_previous_one(self) -> None:
        self.repo.commit("Start")
        self.assertIsNone(rn.previous_release(self.repo.root, "HEAD", "v0.1.0-pre.1"))
        text = rn.compose("STATE", "BODY", "v0.1.0-pre.1", None, [], "o/r")
        self.assertIn("The first pre-release.", text)

    def test_changes_since_the_last_release(self) -> None:
        self.repo.commit("Old work")
        self.repo.tag("v0.1.0-pre.9")
        self.repo.commit("Older than the last one")
        self.repo.tag("v0.1.0-pre.10")
        self.repo.commit("Add sound\n\nLonger text.\n\nRelease-Note: Sound plays.\n"
                         "Release-Note: The menu has a volume setting.")
        self.repo.commit("Fix a typo\n\nRelease-Note: none")
        self.repo.commit("Saves are kept")
        since = rn.previous_release(self.repo.root, "HEAD", "v0.1.0-pre.12")
        self.assertEqual(since, "v0.1.0-pre.10")  # By number, not by name.
        self.assertEqual(rn.changes(self.repo.root, since, "HEAD"),
                         ["Saves are kept", "Sound plays.", "The menu has a volume setting."])

    def test_a_rerun_does_not_count_its_own_tag(self) -> None:
        self.repo.commit("One")
        self.repo.tag("v0.1.0-pre.3")
        self.repo.commit("Two")
        self.repo.tag("v0.1.0-pre.4")
        self.assertEqual(rn.previous_release(self.repo.root, "HEAD", "v0.1.0-pre.4"), "v0.1.0-pre.3")

    def test_other_tags_are_not_releases(self) -> None:
        self.repo.commit("One")
        self.repo.tag("v0.1.0-pre.2")
        self.repo.commit("Two")
        self.repo.tag("experiment")
        self.repo.tag("v0.1.0-pre.2-fix")
        self.assertEqual(rn.previous_release(self.repo.root, "HEAD", "v0.1.0-pre.5"), "v0.1.0-pre.2")

    def test_order_state_then_changes_then_body(self) -> None:
        text = rn.compose("STATE\n", "\nBODY", "v1-pre.2", "v1-pre.1", ["A", "B"], "o/r")
        self.assertLess(text.index("STATE"), text.index("## Changes since v1-pre.1"))
        self.assertLess(text.index("- B"), text.index("BODY"))
        self.assertIn("https://github.com/o/r/compare/v1-pre.1...v1-pre.2", text)

    def test_nothing_new_is_said_so(self) -> None:
        self.repo.commit("One")
        self.repo.tag("v0.1.0-pre.1")
        self.repo.commit("Tidy\n\nRelease-Note: none")
        entries = rn.changes(self.repo.root, "v0.1.0-pre.1", "HEAD")
        self.assertEqual(entries, [])
        text = rn.compose("S", "B", "v0.1.0-pre.2", "v0.1.0-pre.1", entries, None)
        self.assertIn("- Nothing that changes what the program does.", text)


if __name__ == "__main__":
    unittest.main()
