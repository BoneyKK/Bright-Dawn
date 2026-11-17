"""Tests of render_map.py.

Run with: python3 -m unittest discover -s tools/status
"""

from __future__ import annotations

import json
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import render_map as rm


class RenderMapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.data = rm.load()

    def test_the_repository_is_up_to_date(self) -> None:
        self.assertEqual(rm.main(["--check"]), 0)

    def test_the_svg_is_well_formed_and_holds_every_box(self) -> None:
        svg = rm.github_svg(self.data)
        root = ET.fromstring(svg)
        texts = [t.text for t in root.iter("{http://www.w3.org/2000/svg}text")]
        for box in self.data["diagram"]["boxes"]:
            self.assertIn(box["label"], texts)
        for name in self.data["statuses"].values():
            self.assertIn(name, texts)

    def test_readme_part_has_every_part_and_step(self) -> None:
        part = rm.readme_part(self.data)
        self.assertTrue(part.startswith("<!-- project-map:start"))
        self.assertTrue(part.endswith(rm.END))
        for row in self.data["parts"]:
            self.assertIn(f"| `{row['part']}` |", part)
        for n, step in enumerate(self.data["next"], 1):
            self.assertIn(f"{n}. {step}", part)
        self.assertIn(self.data["artifact"], part)

    def test_page_has_every_part_and_step(self) -> None:
        page = rm.page(self.data)
        for row in self.data["parts"]:
            self.assertIn(rm.e(row["now"]), page)
        for step in self.data["next"]:
            self.assertIn(rm.e(step), page)
        self.assertIn(rm.long_date(self.data["last_change"]["date"]), page)

    def test_only_the_part_between_the_markers_changes(self) -> None:
        readme = "Before\n<!-- project-map:start: old -->\nold\n<!-- project-map:end -->\nAfter\n"
        new = rm.with_readme_part(readme, "<!-- project-map:start -->NEW" + rm.END)
        self.assertEqual(new, "Before\n<!-- project-map:start -->NEW<!-- project-map:end -->\nAfter\n")
        with self.assertRaises(rm.MapError):
            rm.with_readme_part("No markers\n", "x")

    def test_text_is_escaped(self) -> None:
        self.data["parts"][0]["now"] = "a | b <c> & d"
        self.data["diagram"]["boxes"][0]["label"] = "<&>"
        self.assertIn("a \\| b <c> & d", rm.readme_part(self.data))
        self.assertIn("&lt;&amp;&gt;", rm.github_svg(self.data))
        self.assertIn("a | b &lt;c&gt; &amp; d", rm.page(self.data))

    def test_unknown_status_or_missing_field_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "map.json"
            bad = json.loads(json.dumps(self.data))
            bad["parts"][0]["status"] = "maybe"
            path.write_text(json.dumps(bad))
            with self.assertRaises(rm.MapError):
                rm.load(path)
            del bad["next"]
            path.write_text(json.dumps(bad))
            with self.assertRaises(rm.MapError):
                rm.load(path)

    def test_dates_read_as_words(self) -> None:
        self.assertEqual(rm.long_date("2026-11-07"), "7 November 2026")


if __name__ == "__main__":
    unittest.main()
