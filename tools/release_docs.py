#!/usr/bin/env python3
"""Writes a new release into the files that name the current release, so nobody does it by hand.

    python3 -B tools/release_docs.py v0.6.0 --date 2026-12-01 --zip-sha256 <hex> --enaga 0.4.0 \
        [--enaga-zip-sha256 <hex> --enaga-license-sha256 <hex>] [--root DIR]

tools/publish_release.sh runs it in step 4, once the zip exists, and commits the result with the
filled notes on release/<tag>, so the release PR brings it into main. It changes:

  - web/public/compat.json: the release pinned at the top of the card builder's list (its zip's
    SHA-256, date and Enaga), so strata.einblick.dev offers it even when GitHub's API refuses the
    site. A new Enaga version is added too, which needs its zip's and LICENSE's SHA-256.
  - README.md: the line under "## Status" after the release-status marker, and the STRATA zip's
    name in "Try it".
  - docs/INSTALL.md: the STRATA zip's name. Its table of which Enaga goes with which STRATA is
    written by hand before the release (the prose around it depends on the release); this script
    refuses when the table does not list the tag yet.

It refuses rather than guesses: a release already pinned or older than the newest pin, an Enaga
that is not pinned and comes without its hashes, a README without the marker. Nothing is written
unless every file can be updated.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO_URL = "https://github.com/Devko/STRATA"
MARKER = "<!-- release-status: tools/publish_release.sh rewrites the next line -->"
ZIP_NAME = re.compile(r"STRATA-v\d+\.\d+\.\d+-smpltrek\.zip")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class Refused(Exception):
    pass


def key(version):
    return tuple(int(n) for n in version.split("."))


def pin_release(text, version, date, zip_sha, enaga, enaga_zip_sha=None, enaga_license_sha=None):
    """compat.json's text with the release pinned at the top of the list."""
    data = json.loads(text)
    strata = data["strata"]
    if any(s["version"] == version for s in strata):
        raise Refused(f"compat.json pins {version} already")
    if strata and key(version) <= key(strata[0]["version"]):
        raise Refused(f"{version} is not newer than {strata[0]['version']}, the newest in compat.json")
    if enaga not in data["enaga"]:
        if not (enaga_zip_sha and enaga_license_sha):
            raise Refused(f"Enaga {enaga} is not in compat.json: it needs --enaga-zip-sha256 and "
                          "--enaga-license-sha256")
        for name, value in (("--enaga-zip-sha256", enaga_zip_sha), ("--enaga-license-sha256", enaga_license_sha)):
            if not HEX64.match(value):
                raise Refused(f"{name}: a SHA-256 in lowercase hex, not {value!r}")
        entry = {"tag": f"v{enaga}", "asset": f"enaga-v{enaga}-smpltrek.zip",
                 "sha256": enaga_zip_sha, "license_sha256": enaga_license_sha}
        # Newest first, as the file keeps them.
        data["enaga"] = dict(sorted({**data["enaga"], enaga: entry}.items(),
                                    key=lambda kv: key(kv[0]), reverse=True))
    strata.insert(0, {"version": version, "tag": f"v{version}", "date": date,
                      "asset": f"STRATA-v{version}-smpltrek.zip", "sha256": zip_sha, "enaga": enaga})
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def status_line(version, date, enaga):
    return (f"**[v{version}]({REPO_URL}/releases/tag/v{version})**, released {date}, is the current "
            f"release, for **Enaga {enaga}**.")


def update_readme(text, version, date, enaga):
    lines = text.split("\n")
    at = [i for i, line in enumerate(lines) if line.strip() == MARKER]
    if len(at) != 1 or at[0] + 1 >= len(lines):
        raise Refused(f"README.md needs the line {MARKER!r} once, above the status line")
    lines[at[0] + 1] = status_line(version, date, enaga)
    text = "\n".join(lines)
    if not ZIP_NAME.search(text):
        raise Refused("README.md names no STRATA-vX.Y.Z-smpltrek.zip to update")
    return ZIP_NAME.sub(f"STRATA-v{version}-smpltrek.zip", text)


def update_install(text, version):
    if not re.search(rf"^\|[^|\n]*\bv{re.escape(version)}\b", text, re.M):
        raise Refused(f"docs/INSTALL.md's table of which Enaga goes with which STRATA does not list "
                      f"v{version}: add it on main before the release")
    if not ZIP_NAME.search(text):
        raise Refused("docs/INSTALL.md names no STRATA-vX.Y.Z-smpltrek.zip to update")
    return ZIP_NAME.sub(f"STRATA-v{version}-smpltrek.zip", text)


def update(root, tag, date, zip_sha, enaga, enaga_zip_sha=None, enaga_license_sha=None):
    """Update the three files under root; returns their paths. Writes nothing on a refusal."""
    m = re.fullmatch(r"v(\d+\.\d+\.\d+)", tag)
    if not m:
        raise Refused(f"the tag should look like v0.1.0, not {tag}")
    version = m.group(1)
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", date):
        raise Refused(f"--date YYYY-MM-DD, not {date}")
    if not HEX64.match(zip_sha):
        raise Refused(f"--zip-sha256: a SHA-256 in lowercase hex, not {zip_sha!r}")
    if not re.fullmatch(r"\d+\.\d+\.\d+", enaga):
        raise Refused(f"--enaga X.Y.Z, not {enaga}")
    root = Path(root)
    paths = {name: root / name for name in ("web/public/compat.json", "README.md", "docs/INSTALL.md")}
    old = {name: p.read_text(encoding="utf-8") for name, p in paths.items()}
    new = {
        "web/public/compat.json": pin_release(old["web/public/compat.json"], version, date, zip_sha,
                                              enaga, enaga_zip_sha, enaga_license_sha),
        "README.md": update_readme(old["README.md"], version, date, enaga),
        "docs/INSTALL.md": update_install(old["docs/INSTALL.md"], version),
    }
    for name, p in paths.items():
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(new[name])
    return list(paths.values())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag")
    ap.add_argument("--date", required=True)
    ap.add_argument("--zip-sha256", required=True)
    ap.add_argument("--enaga", required=True, help="the Enaga version the release needs, X.Y.Z")
    ap.add_argument("--enaga-zip-sha256")
    ap.add_argument("--enaga-license-sha256")
    ap.add_argument("--root", default=str(ROOT))
    a = ap.parse_args(argv)
    try:
        for p in update(a.root, a.tag, a.date, a.zip_sha256, a.enaga, a.enaga_zip_sha256,
                        a.enaga_license_sha256):
            print(f"updated {p}")
    except Refused as e:
        print(f"release_docs: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
