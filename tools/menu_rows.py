#!/usr/bin/env python3
"""Where a row sits in one of the app's menus, read from the enum that orders it.

    python3 -B tools/menu_rows.py app/ui_perfmix.hpp MenuItem chain        -> 11
    python3 -B tools/menu_rows.py app/ui_trackmenu.hpp TrackRow play --up  -> 4

Scenarios walk menus with key presses, and a count written into the scenario broke every time a
row was added above the target (TRANSPOSE in the FUNC menu, CAPTURE in the track menu: four
scenarios failed on main, 2026-11-28). The rows are in the order of their enum, so the count is
read from it instead: the index (DOWN presses from the first row), or with --up the UP presses
from the first row going round (count - index). The enum's last member, `count`, is not a row.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def rows(header, enum):
    text = (ROOT / header).read_text()
    match = re.search(r"enum\s+class\s+" + re.escape(enum) + r"\b[^{]*\{([^}]*)\}", text)
    if not match:
        raise SystemExit(f"menu_rows: no enum class {enum} in {header}")
    body = re.sub(r"//[^\n]*", "", match.group(1))
    names = [n.split("=")[0].strip() for n in body.split(",")]
    return [n for n in names if n and n != "count"]


def down(header, enum, row):
    """DOWN presses from the first row to `row`."""
    names = rows(header, enum)
    if row not in names:
        raise SystemExit(f"menu_rows: {enum} has no row {row} ({', '.join(names)})")
    return names.index(row)


def up(header, enum, row):
    """UP presses from the first row to `row`, going round past the last."""
    return (len(rows(header, enum)) - down(header, enum, row)) % len(rows(header, enum))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--up"]
    if len(args) != 3:
        raise SystemExit(__doc__)
    print((up if "--up" in sys.argv else down)(*args))
