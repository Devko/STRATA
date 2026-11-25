#!/usr/bin/env python3
"""Turn a finished SD-card folder into a STRATA release zip.

    python3 -B tools/package_release.py v0.1.0                      # build/card -> build/release/
    python3 -B tools/package_release.py v0.1.0 --card D:/sd --out /tmp/rel
    python3 -B tools/package_release.py v0.1.0 --draft              # a trial zip before the notes are final

The card folder is what went on the SD card for the unit test. Only one file is taken from it:

    <card>/ENAGA/APPS/STRATA/APP.ENA        the STRATA app

The zip never carries firmware. Enaga's SmplTrek.bin (and anything else on the card: STRATA6,
test samples, projects) is listed and left out; the zip's README.txt links Enaga's own release
of the pinned version instead, so users install Enaga from its authors and not from a copy of
ours. The app is checked before anything is written, and the script refuses an APP.ENA that is
not an Enaga package, is not named STRATA, was built against another ENAGA_ABI_VERSION than the
pinned Enaga's, or whose payload CRC is wrong. As a last check it reads the finished zip back and
refuses if any firmware file (*.bin) is in it.

It also refuses while CHANGELOG.md still has a TODO(release), an unfilled {{FIELD}}, or no date,
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
ENAGA_RELEASES = "https://github.com/seawaffle/enaga/releases/tag/v{e}"
ZIP_DATE = (2026, 1, 1, 0, 0, 0)

PACKAGE_MAGIC = 0x41474E45          # "ENGA", ENAGA_PACKAGE_MAGIC in <enaga/abi.h>


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


# --- the zip -------------------------------------------------------------------------------

def readme_text(release, app_version, enaga_version):
    template = (ROOT / "docs" / "release" / "README.txt").read_text(encoding="utf-8")
    text = (template.replace("{RELEASE}", release).replace("{APP_VERSION}", str(app_version))
            .replace("{ENAGA_URL}", ENAGA_RELEASES.format(e=enaga_version))
            .replace("{ENAGA}", enaga_version))
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
    with zipfile.ZipFile(tmp) as z:
        firmware = [n for n in z.namelist() if n.lower().endswith(".bin")]
    if firmware:
        tmp.unlink()
        raise Refused(f"the zip would carry firmware ({', '.join(firmware)}): STRATA releases never do")
    tmp.replace(out_zip)


def check_notes(release, changelog):
    """Return the reasons the changelog is not final for `release` (empty when it is)."""
    reasons = []
    if "TODO(release)" in changelog:
        reasons.append("CHANGELOG.md still has TODO(release)")
    fields = sorted(set(re.findall(r"\{\{[A-Z_0-9]+\}\}", changelog)))
    if fields:
        reasons.append(f"CHANGELOG.md has unfilled fields {', '.join(fields)} (tools/publish_release.sh fills them)")
    version = release[1:]
    if not re.search(r"^## \[" + re.escape(version) + r"\] - \d{4}-\d{2}-\d{2}\s*$", changelog, re.M):
        reasons.append(f"CHANGELOG.md has no dated '## [{version}] - YYYY-MM-DD' heading")
    return reasons


def package(release, card, out_dir, draft=False, log=print):
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
    enaga_version = ".".join(map(str, pinned_firmware_version()))

    app_file = card.joinpath(*APP_PATH)
    if not app_file.is_file():
        raise Refused(f"no {'/'.join(APP_PATH)} in {card}")
    app = app_file.read_bytes()
    app_version, _ = check_app(app, abi)
    log(f"APP.ENA        {len(app):>9} bytes  app version {app_version}, ENAGA_ABI_VERSION {abi}, CRC ok")

    for path in sorted(p for p in card.rglob("*") if p.is_file() and p.resolve() != app_file.resolve()):
        log(f"left out      {path.relative_to(card).as_posix()}")

    top = f"STRATA-{release}"
    payload = [("SD-CARD/" + "/".join(APP_PATH), app)]
    sums = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in payload)

    files = [("README.txt", readme_text(release, app_version, enaga_version).encode())]
    files += payload
    files += [
        ("CHANGELOG.md", changelog),
        ("LICENSE.txt", (ROOT / "LICENSE").read_bytes()),
        ("THIRD_PARTY_NOTICES.md", (ROOT / "THIRD_PARTY_NOTICES.md").read_bytes()),
        ("SHA256SUMS.txt", sums.encode()),
    ]
    suffix = "-draft" if draft else ""
    out_zip = Path(out_dir) / f"{top}-smpltrek{suffix}.zip"
    build_zip(out_zip, top, files)
    digest = hashlib.sha256(out_zip.read_bytes()).hexdigest()
    (out_zip.parent / (out_zip.name + ".sha256")).write_text(f"{digest}  {out_zip.name}\n")
    log(sums.rstrip())
    log(f"Enaga          not in the zip: README.txt links {ENAGA_RELEASES.format(e=enaga_version)}")
    log(f"RELEASE OK  {out_zip}  {out_zip.stat().st_size} bytes  sha256 {digest}")
    return out_zip


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("release", help="the release tag, e.g. v0.1.0")
    ap.add_argument("--card", default=str(ROOT / "build" / "card"), help="the SD-card folder (default build/card)")
    ap.add_argument("--out", default=str(ROOT / "build" / "release"), help="where the zip goes (default build/release)")
    ap.add_argument("--draft", action="store_true", help="package although the changelog is not final")
    args = ap.parse_args(argv)
    try:
        package(args.release, args.card, args.out, args.draft)
    except Refused as e:
        print(f"package_release: refused: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
