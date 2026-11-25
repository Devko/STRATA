#!/usr/bin/env python3
"""Tests for tools/package_release.py. Standard library only:

    python3 -B tests/test_package_release.py

Builds fake card folders in a temporary directory: an APP.ENA with a real package header and
CRC, and, as the unit-test card has one, an Enaga SmplTrek.bin that must never reach the zip.
"""
import hashlib
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import package_release as pr  # noqa: E402

VERSION = pr.pinned_firmware_version()
FIRMWARE = b"cmtd" + b"\0" * 8188      # stands in for Enaga's SmplTrek.bin; never read, only left out
ABI = pr.pinned_abi_version()


def make_app(name="STRATA", abi=ABI, payload=b"\x01\x02\x03\x04" * 64, app_version=1, crc=None):
    header = struct.pack("<10I", pr.PACKAGE_MAGIC, 64, abi, 0xC1000000, len(payload), 128, 0,
                         zlib.crc32(payload) if crc is None else crc, app_version, 0)
    return header + name.encode().ljust(24, b"\0") + payload


def silent(*_):
    pass


class PackageRelease(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.card = self.dir / "card"
        self.out = self.dir / "out"

    def tearDown(self):
        self.tmp.cleanup()

    def put(self, rel, data):
        path = self.card / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def good_card(self):
        self.put("SmplTrek.bin", FIRMWARE)
        self.put("ENAGA/APPS/STRATA/APP.ENA", make_app())

    def package(self, **kw):
        kw.setdefault("draft", True)
        return pr.package("v0.1.0", self.card, self.out, log=kw.pop("log", silent), **kw)

    def refused(self, text, **kw):
        with self.assertRaises(pr.Refused) as cm:
            self.package(**kw)
        self.assertIn(text, str(cm.exception))

    # --- accepted ---------------------------------------------------------------------------

    def test_a_good_card_gives_the_expected_zip(self):
        self.good_card()
        out = self.package()
        self.assertEqual(out.name, "STRATA-v0.1.0-smpltrek-draft.zip")
        with zipfile.ZipFile(out) as z:
            names = z.namelist()
            self.assertEqual(names, [
                "STRATA-v0.1.0/README.txt",
                "STRATA-v0.1.0/SD-CARD/ENAGA/APPS/STRATA/APP.ENA",
                "STRATA-v0.1.0/CHANGELOG.md",
                "STRATA-v0.1.0/LICENSE.txt",
                "STRATA-v0.1.0/THIRD_PARTY_NOTICES.md",
                "STRATA-v0.1.0/SHA256SUMS.txt",
            ])
            self.assertEqual(z.read(names[1]), make_app())
            readme = z.read(names[0]).decode()
            self.assertIn("STRATA v0.1.0", readme)
            self.assertIn("Enaga " + ".".join(map(str, VERSION)), readme)
            self.assertIn("https://github.com/seawaffle/enaga/releases/tag/v" + ".".join(map(str, VERSION)), readme)
            self.assertNotIn("{", readme)
            self.assertIn("\r\n", readme)
            sums = z.read(names[5]).decode()
            self.assertEqual(sums.count("\n"), 1)
            self.assertIn(hashlib.sha256(make_app()).hexdigest() + "  SD-CARD/ENAGA/APPS/STRATA/APP.ENA", sums)
        self.assertTrue((self.out / (out.name + ".sha256")).is_file())

    def test_the_zip_is_reproducible(self):
        self.good_card()
        first = self.package().read_bytes()
        second = self.package().read_bytes()
        self.assertEqual(first, second)

    def test_other_files_on_the_card_are_left_out(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA6/APP.ENA", make_app("STRATA6"))
        self.put("ENAGA/DATA/STRATA/PROJECT.BIN", b"x")
        with zipfile.ZipFile(self.package()) as z:
            self.assertFalse(any("STRATA6" in n or "DATA" in n for n in z.namelist()))

    def test_the_firmware_on_the_card_is_left_out(self):
        self.good_card()
        logged = []
        with zipfile.ZipFile(self.package(log=logged.append)) as z:
            self.assertFalse(any(n.lower().endswith(".bin") for n in z.namelist()))
            self.assertFalse(any(FIRMWARE == z.read(n) for n in z.namelist()))
        self.assertIn("left out      SmplTrek.bin", logged)

    def test_a_card_without_firmware_packages_the_same(self):
        self.good_card()
        with_bin = self.package().read_bytes()
        (self.card / "SmplTrek.bin").unlink()
        self.assertEqual(self.package().read_bytes(), with_bin)

    def test_a_zip_that_would_carry_firmware_is_refused(self):
        with self.assertRaises(pr.Refused) as cm:
            pr.build_zip(self.out / "x.zip", "STRATA-v0.1.0", [("SD-CARD/SmplTrek.bin", FIRMWARE)])
        self.assertIn("never", str(cm.exception))
        self.assertFalse(any(self.out.iterdir()))

    # --- refused ----------------------------------------------------------------------------

    def test_a_missing_app_is_refused(self):
        self.put("SmplTrek.bin", FIRMWARE)
        self.refused("no ENAGA/APPS/STRATA/APP.ENA")

    def test_a_test_build_under_strata_is_refused(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA/APP.ENA", make_app("STRATA6"))
        self.refused("STRATA6")

    def test_another_abi_is_refused(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA/APP.ENA", make_app(abi=ABI + 1))
        self.refused("ENAGA_ABI_VERSION")

    def test_a_damaged_app_is_refused(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA/APP.ENA", make_app(crc=1))
        self.refused("CRC is wrong")

    def test_a_truncated_app_is_refused(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA/APP.ENA", make_app()[:100])
        self.refused("truncated")

    def test_not_an_app_is_refused(self):
        self.good_card()
        self.put("ENAGA/APPS/STRATA/APP.ENA", b"\0" * 200)
        self.refused("not an Enaga package")

    def test_a_bad_release_name_is_refused(self):
        self.good_card()
        with self.assertRaises(pr.Refused):
            pr.package("0.1", self.card, self.out, draft=True, log=silent)

    # --- the notes gate ---------------------------------------------------------------------

    def test_notes_gate(self):
        final = "# Changelog\n\n## [0.1.0] - 2026-11-23\n\nText.\n"
        self.assertEqual(pr.check_notes("v0.1.0", final), [])
        self.assertEqual(len(pr.check_notes("v0.1.0", final + "TODO(release)\n")), 1)
        self.assertEqual(len(pr.check_notes("v0.1.0", final.replace("2026-11-23", "unreleased"))), 1)
        self.assertEqual(len(pr.check_notes("v0.2.0", final)), 1)
        self.assertEqual(len(pr.check_notes("v0.1.0", final + "MAX {{MAX_A01}}\n")), 1)
        self.assertEqual(len(pr.check_notes("v0.1.0", final.replace("2026-11-23", "{{RELEASE_DATE}}"))), 2)

    def test_an_unfinished_changelog_needs_draft(self):
        self.good_card()
        if not pr.check_notes("v0.1.0", (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")):
            self.skipTest("CHANGELOG.md is final for v0.1.0")
        self.refused("--draft", draft=False)


if __name__ == "__main__":
    unittest.main(verbosity=1)
