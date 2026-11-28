#!/usr/bin/env python3
"""Tests for tools/release_docs.py. Standard library only:

    python3 -B tests/test_release_docs.py

Runs on copies of the repository's own compat.json, README.md and INSTALL.md, so a change to
those files that would break the release script fails here first.
"""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_docs as rd  # noqa: E402

FILES = ("web/public/compat.json", "README.md", "docs/INSTALL.md")
SHA = "ab" * 32
SHA2 = "cd" * 32
SHA3 = "ef" * 32


def newest():
    return json.loads((ROOT / FILES[0]).read_text(encoding="utf-8"))["strata"][0]


def bump(version):
    a, b, c = map(int, version.split("."))
    return f"{a}.{b}.{c + 1}"


class ReleaseDocs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        for name in FILES:
            (self.root / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, self.root / name)
        self.last = newest()
        self.version = bump(self.last["version"])
        self.tag = f"v{self.version}"
        # The table row is written by hand before a release; stand in for that.
        install = self.root / "docs/INSTALL.md"
        text = install.read_text(encoding="utf-8")
        text = text.replace("| --- | --- |\n| ", f"| --- | --- |\n| {self.tag}, ", 1)
        install.write_text(text, encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def read(self, name):
        return (self.root / name).read_text(encoding="utf-8")

    def update(self, **kw):
        args = dict(tag=self.tag, date="2026-12-01", zip_sha=SHA, enaga=self.last["enaga"])
        args.update(kw)
        return rd.update(self.root, **args)

    def refused(self, text, **kw):
        before = {n: self.read(n) for n in FILES}
        with self.assertRaises(rd.Refused) as cm:
            self.update(**kw)
        self.assertIn(text, str(cm.exception))
        for n in FILES:
            self.assertEqual(self.read(n), before[n], f"{n} changed although the update was refused")

    def test_pins_the_release_at_the_top(self):
        self.update()
        data = json.loads(self.read(FILES[0]))
        self.assertEqual(data["strata"][0], {
            "version": self.version, "tag": self.tag, "date": "2026-12-01",
            "asset": f"STRATA-{self.tag}-smpltrek.zip", "sha256": SHA, "enaga": self.last["enaga"]})
        self.assertEqual(data["strata"][1], self.last)
        self.assertTrue(self.read(FILES[0]).endswith("}\n"))

    def test_only_the_new_entry_is_added(self):
        before = self.read(FILES[0])
        self.update()
        after = self.read(FILES[0])
        self.assertEqual(json.dumps(json.loads(before), indent=2, ensure_ascii=False) + "\n", before,
                         "compat.json is not in the layout the script writes")
        self.assertEqual(len(after.splitlines()) - len(before.splitlines()), 8)

    def test_readme_status_and_zip_name(self):
        self.update()
        text = self.read("README.md")
        lines = text.split("\n")
        line = lines[lines.index(rd.MARKER) + 1]
        self.assertEqual(line, f"**[{self.tag}](https://github.com/Devko/STRATA/releases/tag/{self.tag})**, "
                               f"released 2026-12-01, is the current release, for **Enaga {self.last['enaga']}**.")
        self.assertIn(f"`STRATA-{self.tag}-smpltrek.zip`", text)
        self.assertNotIn(f"STRATA-v{self.last['version']}-smpltrek.zip", text)

    def test_install_zip_names(self):
        self.update()
        text = self.read("docs/INSTALL.md")
        self.assertIn(f"STRATA-{self.tag}-smpltrek.zip", text)
        self.assertNotIn(f"STRATA-v{self.last['version']}-smpltrek.zip", text)

    def test_the_repository_readme_has_the_current_release(self):
        text = (ROOT / "README.md").read_text(encoding="utf-8")
        lines = text.split("\n")
        self.assertIn(rd.MARKER, lines, "README.md lost the release-status marker")
        self.assertEqual(lines[lines.index(rd.MARKER) + 1],
                         rd.status_line(self.last["version"], self.last["date"], self.last["enaga"]),
                         "README.md's status line does not name the newest release in compat.json")

    def test_a_new_enaga_needs_its_hashes(self):
        self.refused("is not in compat.json", enaga="9.9.9")
        self.refused("lowercase hex", enaga="9.9.9", enaga_zip_sha="nope", enaga_license_sha=SHA3)
        self.update(enaga="9.9.9", enaga_zip_sha=SHA2, enaga_license_sha=SHA3)
        data = json.loads(self.read(FILES[0]))
        self.assertEqual(next(iter(data["enaga"])), "9.9.9", "Enaga versions stay newest first")
        self.assertEqual(data["enaga"]["9.9.9"], {"tag": "v9.9.9", "asset": "enaga-v9.9.9-smpltrek.zip",
                                                  "sha256": SHA2, "license_sha256": SHA3})
        self.assertIn("**Enaga 9.9.9**", self.read("README.md"))

    def test_refusals(self):
        self.refused("pins", tag=f"v{self.last['version']}")
        self.refused("not newer", tag="v0.0.1")
        self.refused("lowercase hex", zip_sha="AB" * 32)
        self.refused("the tag should look like", tag="0.9.0")
        self.refused("--date", date="1.12.2026")
        self.refused("does not list", tag=f"v{bump(self.version)}")

    def test_a_readme_without_the_marker_is_refused(self):
        readme = self.root / "README.md"
        readme.write_text(self.read("README.md").replace(rd.MARKER, ""), encoding="utf-8")
        self.refused("release-status")

    def test_command_line(self):
        self.assertEqual(rd.main([self.tag, "--date", "2026-12-01", "--zip-sha256", SHA,
                                  "--enaga", self.last["enaga"], "--root", str(self.root)]), 0)
        self.assertEqual(rd.main([self.tag, "--date", "2026-12-01", "--zip-sha256", SHA,
                                  "--enaga", self.last["enaga"], "--root", str(self.root)]), 1)


if __name__ == "__main__":
    unittest.main(verbosity=1)
