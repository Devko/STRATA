#!/usr/bin/env python3
"""Turn a finished SD-card folder into a STRATA release zip.

    python3 -B tools/package_release.py v0.1.0                      # build/card -> build/release/
    python3 -B tools/package_release.py v0.1.0 --card D:/sd --out /tmp/rel
    python3 -B tools/package_release.py v0.1.0 --no-firmware        # APP.ENA only, link to Enaga
    python3 -B tools/package_release.py v0.1.0 --draft              # a trial zip before the notes are final

The card folder is what goes on the SD card for a unit test:

    <card>/SmplTrek.bin                     Enaga's SmplTrek update file
    <card>/ENAGA/APPS/STRATA/APP.ENA        the STRATA app

Only those two files are taken. Anything else on the card (STRATA6, test samples, projects) is
listed and left out. Both files are checked before anything is written, and the script refuses:

- an APP.ENA that is not an Enaga package, is not named STRATA, was built against another
  ENAGA_ABI_VERSION than the pinned Enaga's, or whose payload CRC is wrong;
- a SmplTrek.bin that is not exactly one SYSTEM image from Enaga at the pinned version, for the
  SmplTrek, with both CRCs right. An official Sonicware update file fails this (its version is
  3.x and it carries more than one section), and Sonicware's firmware must never be redistributed.
  So does any file with BOOT or MCU sections.

It also refuses while CHANGELOG.md still has a TODO(release) or calls the release "unreleased",
unless --draft is given; a draft zip is named ...-draft.zip so it cannot be mistaken for the real one.

The zip is reproducible: fixed timestamps and order, so the same inputs give the same bytes.
Python 3.8+, standard library only, runs on Windows or Linux.
"""
import argparse
import hashlib
import re
import struct
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENAGA = ROOT / "third_party" / "enaga"
APP_NAME = "STRATA"
APP_PATH = ("ENAGA", "APPS", APP_NAME, "APP.ENA")
FIRMWARE_NAME = "SmplTrek.bin"
ZIP_DATE = (2026, 1, 1, 0, 0, 0)

PACKAGE_MAGIC = 0x41474E45          # "ENGA", ENAGA_PACKAGE_MAGIC in <enaga/abi.h>
SYSTEM_KIND = 3                     # the cmtd descriptor kind for a SYSTEM image
SMPLTREK_FAMILY = 5                 # tools/sysimage.py DEVICES["smpltrek"]["family"]


class Refused(Exception):
    pass


# --- what the pinned Enaga says ------------------------------------------------------------

def pinned_abi_version():
    text = (ENAGA / "include" / "enaga" / "abi.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+ENAGA_ABI_VERSION\s+(\d+)u?", text)
    if not m:
        raise Refused("cannot read ENAGA_ABI_VERSION from third_party/enaga (submodule not checked out?)")
    return int(m.group(1))


def pinned_firmware_version():
    text = (ENAGA / "firmware" / "build.py").read_text(encoding="utf-8")
    m = re.search(r"^VERSION\s*=\s*\((\d+),\s*(\d+),\s*(\d+)\)", text, re.M)
    if not m:
        raise Refused("cannot read VERSION from third_party/enaga/firmware/build.py")
    return tuple(int(n) for n in m.groups())


def sysimage():
    sys.path.insert(0, str(ENAGA / "tools"))
    try:
        import sysimage as module      # Enaga's own reader: the CRC seeds live there
    finally:
        sys.path.pop(0)
    return module


# --- checks --------------------------------------------------------------------------------

def check_app(data, abi_version):
    """Return (app_version, name) for a valid STRATA package, or raise Refused."""
    if len(data) < 64:
        raise Refused(f"APP.ENA is {len(data)} bytes, shorter than a package header")
    magic, header_size, abi, _load, load_size, _bss, _entry, crc, app_version, _flags = \
        struct.unpack_from("<10I", data, 0)
    name = data[40:64].split(b"\0", 1)[0].decode("ascii", "replace")
    if magic != PACKAGE_MAGIC:
        raise Refused("APP.ENA is not an Enaga package (wrong magic)")
    if abi != abi_version:
        raise Refused(f"APP.ENA was built against ENAGA_ABI_VERSION {abi}, the pinned Enaga is {abi_version}")
    if name != APP_NAME:
        raise Refused(f"APP.ENA is named {name!r}, not {APP_NAME!r} (a test build such as STRATA6?)")
    payload = data[header_size:header_size + load_size]
    if len(payload) != load_size:
        raise Refused("APP.ENA is truncated: the payload is shorter than its header says")
    if zlib.crc32(payload) != crc:
        raise Refused("APP.ENA's payload CRC is wrong: the file is damaged")
    return app_version, name


def check_firmware(data, version, si):
    """Raise Refused unless `data` is one SmplTrek SYSTEM image made by Enaga at `version`."""
    if len(data) < 84 or data[:4] != b"cmtd" or data[48:52] != b"mmtd":
        raise Refused(f"{FIRMWARE_NAME} is not a SmplTrek update file")
    family, descriptors = struct.unpack_from("<II", data, 16)
    kind = struct.unpack_from("<I", data, 24)[0]
    file_version = struct.unpack_from("<3I", data, 28)
    image_version = struct.unpack_from("<3I", data, 48 + 16)
    if family != SMPLTREK_FAMILY:
        raise Refused(f"{FIRMWARE_NAME} is for device family {family}, not the SmplTrek")
    if descriptors != 1 or kind != SYSTEM_KIND:
        raise Refused(f"{FIRMWARE_NAME} holds {descriptors} image(s), the first of kind {kind}: only one SYSTEM "
                      "image is allowed (no BOOT or MCU sections; an official Sonicware update is refused here)")
    if file_version != version or image_version != version:
        found = ".".join(map(str, file_version))
        raise Refused(f"{FIRMWARE_NAME} is version {found}, the pinned Enaga is {'.'.join(map(str, version))}"
                      " (an official Sonicware update file must never be redistributed)")
    if not si.verify(data, "smpltrek"):
        raise Refused(f"{FIRMWARE_NAME}'s CRCs are wrong or its size does not match: the file is damaged")


# --- the zip -------------------------------------------------------------------------------

def readme_text(release, app_version, firmware, enaga_version):
    template = (ROOT / "docs" / "release" / "README.txt").read_text(encoding="utf-8")
    if firmware:
        contents = ("  SD-CARD/SmplTrek.bin                   Enaga v{e}, the SmplTrek update file\n"
                    "  SD-CARD/ENAGA/APPS/STRATA/APP.ENA      the STRATA app").format(e=enaga_version)
        step1 = ("Copy SD-CARD/SmplTrek.bin to the ROOT of the SD card. Hold HOME (F4) while\n"
                 "   switching on to reach BOOT MODE, and choose SYSTEM UPDATE.")
    else:
        contents = "  SD-CARD/ENAGA/APPS/STRATA/APP.ENA      the STRATA app"
        step1 = ("Download the SmplTrek zip from Enaga's v{e} release:\n"
                 "     https://github.com/seawaffle/enaga/releases/tag/v{e}\n"
                 "   Copy its SmplTrek.bin to the ROOT of the SD card. Hold HOME (F4) while\n"
                 "   switching on to reach BOOT MODE, and choose SYSTEM UPDATE.").format(e=enaga_version)
    text = (template.replace("{RELEASE}", release).replace("{APP_VERSION}", str(app_version))
            .replace("{ENAGA}", enaga_version).replace("{CONTENTS}", contents).replace("{STEP1}", step1))
    left = re.findall(r"\{[A-Z_0-9]+\}", text)
    if left:
        raise Refused(f"docs/release/README.txt has unfilled placeholders: {', '.join(sorted(set(left)))}")
    return text.replace("\r\n", "\n").replace("\n", "\r\n")      # README.txt is read on Windows too


def build_zip(out_zip, top, files):
    """`files` is [(path inside top, bytes)], written in the given order with fixed metadata."""
    out_zip.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_zip.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name, data in files:
            info = zipfile.ZipInfo(f"{top}/{name}", date_time=ZIP_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    tmp.replace(out_zip)


def check_notes(release, changelog):
    """Return the reasons the changelog is not final for `release` (empty when it is)."""
    reasons = []
    if "TODO(release)" in changelog:
        reasons.append("CHANGELOG.md still has TODO(release)")
    version = release[1:]
    if not re.search(r"^## \[" + re.escape(version) + r"\] - \d{4}-\d{2}-\d{2}\s*$", changelog, re.M):
        reasons.append(f"CHANGELOG.md has no dated '## [{version}] - YYYY-MM-DD' heading")
    return reasons


def package(release, card, out_dir, with_firmware, draft=False, log=print):
    if not re.fullmatch(r"v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", release):
        raise Refused(f"release {release!r} should look like v0.1.0 or v0.1.0-rc1")
    changelog = (ROOT / "CHANGELOG.md").read_bytes()
    reasons = check_notes(release, changelog.decode("utf-8"))
    if reasons and not draft:
        raise Refused("; ".join(reasons) + " (pass --draft for a trial zip)")
    for reason in reasons:
        log(f"draft: {reason}")
    card = Path(card)
    if not card.is_dir():
        raise Refused(f"no card folder at {card}")

    abi = pinned_abi_version()
    version = pinned_firmware_version()
    enaga_version = ".".join(map(str, version))

    app_file = card.joinpath(*APP_PATH)
    if not app_file.is_file():
        raise Refused(f"no {'/'.join(APP_PATH)} in {card}")
    app = app_file.read_bytes()
    app_version, _ = check_app(app, abi)
    log(f"APP.ENA        {len(app):>9} bytes  app version {app_version}, ENAGA_ABI_VERSION {abi}, CRC ok")

    firmware = None
    if with_firmware:
        fw_file = card / FIRMWARE_NAME
        if not fw_file.is_file():
            raise Refused(f"no {FIRMWARE_NAME} in {card} (or pass --no-firmware)")
        firmware = fw_file.read_bytes()
        check_firmware(firmware, version, sysimage())
        log(f"{FIRMWARE_NAME}   {len(firmware):>9} bytes  Enaga {enaga_version}, one SYSTEM image, CRCs ok")

    taken = {app_file.resolve()} | ({(card / FIRMWARE_NAME).resolve()} if firmware else set())
    for path in sorted(p for p in card.rglob("*") if p.is_file() and p.resolve() not in taken):
        log(f"left out      {path.relative_to(card).as_posix()}")

    top = f"STRATA-{release}"
    payload = []
    if firmware:
        payload.append((f"SD-CARD/{FIRMWARE_NAME}", firmware))
    payload.append(("SD-CARD/" + "/".join(APP_PATH), app))
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in payload)

    files = [("README.txt", readme_text(release, app_version, firmware is not None, enaga_version).encode())]
    files += payload
    files += [
        ("CHANGELOG.md", changelog),
        ("LICENSE.txt", (ROOT / "LICENSE").read_bytes()),
        ("THIRD_PARTY_NOTICES.md", (ROOT / "THIRD_PARTY_NOTICES.md").read_bytes()),
        ("SHA256SUMS.txt", sums.encode()),
    ]
    suffix = ("" if firmware else "-app-only") + ("-draft" if draft else "")
    out_zip = Path(out_dir) / f"{top}-smpltrek{suffix}.zip"
    build_zip(out_zip, top, files)
    digest = hashlib.sha256(out_zip.read_bytes()).hexdigest()
    (out_zip.parent / (out_zip.name + ".sha256")).write_text(f"{digest}  {out_zip.name}\n")
    log(sums.rstrip())
    log(f"RELEASE OK  {out_zip}  {out_zip.stat().st_size} bytes  sha256 {digest}")
    return out_zip


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("release", help="the release tag, e.g. v0.1.0")
    ap.add_argument("--card", default=str(ROOT / "build" / "card"), help="the SD-card folder (default build/card)")
    ap.add_argument("--out", default=str(ROOT / "build" / "release"), help="where the zip goes (default build/release)")
    ap.add_argument("--no-firmware", action="store_true", help="ship APP.ENA only and point to Enaga's release")
    ap.add_argument("--draft", action="store_true", help="package although the changelog is not final")
    args = ap.parse_args(argv)
    try:
        package(args.release, args.card, args.out, not args.no_firmware, args.draft)
    except Refused as e:
        print(f"package_release: refused: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
