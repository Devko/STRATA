#!/usr/bin/env python3
"""Which emulator scenarios a change needs: the fast per-PR set behind `tools/scenarios.sh --changed`.

    python3 -B tools/scenario_pick.py [--why] FILE...     the scenario names, one a line
    git diff --name-only origin/main... | python3 -B tools/scenario_pick.py --why -

A full pass is 66 scenarios and, at -j 4, about a quarter of an hour of every core on Roland's
PC; with several threads at once it was most of the working day (2026-11-28). Most changes touch
one feature, so a PR runs the scenarios that feature can break plus a smoke set, and the full
pass runs after a batch of merges, before a card and before a release (docs/SCENARIO_LOG.md).

The rules, in order (the first that matches a file decides for it):

  1. Prose (*.md anywhere but tests/ and tools/, docs/, web/, content/) needs no scenario.
  2. Anything every scenario stands on -- third_party/, tools/env.sh, tools/build.sh,
     tools/enaga_cached.py, tools/emu_drive.py, tools/scenarios.sh, this file -- needs ALL.
  3. tests/scenario_<name>.sh needs <name>.
  4. A source in core/, dsp/ or app/ needs every scenario whose script names it (its path, or its
     stem as a word: dsp/grain.cpp -> "grain", app/ui_trackmenu.hpp -> "trackmenu",
     dsp/valoops.ti.c -> "va") or that is named after its stem. A dsp/ source, a *.ti.c loop or
     the mixer also needs LOAD (the scenarios that read the worst peaks), since any of them can
     move a load figure.
  5. Any other source in core/, dsp/ or app/, and host/, tests/ or tools/ code, needs only SMOKE.

SMOKE runs whenever any code changed: the app boots and the transport runs (m0), the factory
beat's load (factory), the worst load (genload) and every live screen drawing on the C674x
(visuals). Unknown paths fall to rule 5. The selection only ever adds scenarios to SMOKE.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SMOKE = ["m0", "factory", "genload", "visuals"]
LOAD = ["genload", "energy", "ambient", "fm", "grain", "wave"]
EVERYTHING = ("third_party/", "tools/env.sh", "tools/build.sh", "tools/enaga_cached.py",
              "tools/emu_drive.py", "tools/scenarios.sh", "tools/scenario_pick.py",
              "tools/menu_rows.py")
LOAD_SOURCES = ("dsp/", "app/engine.cpp", "app/engine.hpp", "app/mixer.hpp")


def scenarios():
    return sorted(p.name[len("scenario_"):-3] for p in (ROOT / "tests").glob("scenario_*.sh"))


def is_prose(path):
    if path.startswith(("tests/", "tools/", "core/", "dsp/", "app/", "host/", ".github/")):
        return False
    return path.endswith(".md") or path.startswith(("docs/", "web/", "content/"))


def stem(path):
    name = path.rsplit("/", 1)[-1]
    for suffix in (".ti.c", ".cpp", ".hpp", ".c", ".h", ".inc"):
        if name.endswith(suffix):
            name = name[: -len(suffix)]
            break
    if name.startswith("ui_"):
        name = name[3:]
    if name.endswith("loops") and len(name) > len("loops"):
        name = name[: -len("loops")]
    return name


def pick(files):
    """{scenario: reason}, or None for every scenario; {} when nothing needs one."""
    names = scenarios()
    texts = {n: (ROOT / "tests" / f"scenario_{n}.sh").read_text(errors="replace") for n in names}
    chosen = {}
    code = False

    def add(name, why):
        if name in texts:
            chosen.setdefault(name, why)

    for f in files:
        f = f.strip().replace("\\", "/")
        if not f or is_prose(f):
            continue
        code = True
        if f.startswith(EVERYTHING):
            return None
        m = re.fullmatch(r"tests/scenario_([a-z0-9]+)\.sh", f)
        if m:
            add(m.group(1), "changed")
            continue
        if f.startswith(("core/", "dsp/", "app/")):
            s = stem(f)
            word = re.compile(r"\b" + re.escape(s) + r"\b", re.I) if len(s) >= 3 else None
            for n in names:
                if n == s or f in texts[n] or (word and word.search(texts[n])):
                    add(n, f"names {f}")
            if f.startswith(LOAD_SOURCES) or f.endswith(".ti.c"):
                for n in LOAD:
                    add(n, f"load ({f})")
    if not code:
        return {}
    for n in SMOKE:
        add(n, "smoke")
    return chosen


def main(argv):
    why = "--why" in argv
    args = [a for a in argv if a != "--why"]
    files = sys.stdin.read().split() if args == ["-"] else args
    chosen = pick(files)
    if chosen is None:
        for n in scenarios():
            print(f"{n}  (all: a shared harness or build file changed)" if why else n)
        return 0
    for n in sorted(chosen):
        print(f"{n}  ({chosen[n]})" if why else n)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
