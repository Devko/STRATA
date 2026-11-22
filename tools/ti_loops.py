#!/usr/bin/env python3
"""Ask cl6x how it software-pipelined every hot loop STRATA owns, and Enaga's DSP library.

    python3 -B tools/ti_loops.py                       every *.ti.c in core/ and dsp/
    python3 -B tools/ti_loops.py --enaga               and third_party/enaga/runtime/dsp/*.c
    python3 -B tools/ti_loops.py -D STRATA_TI_HINTS=1  with a switch from dsp/tihints.h set
    python3 -B tools/ti_loops.py --keep DIR            keep the annotated assembly in DIR

The C674x has no packed-float instructions; what makes a DSP loop fast on it is the software
pipeliner overlapping iterations across the eight functional units. cl6x reports, per loop, the
two bounds that decide how well that went, and this prints them as one table:

  rec    Loop Carried Dependency Bound: cycles one iteration's result takes to come back round
         (a filter's state, an envelope's level, a phase). Overlap cannot beat it.
  res    Partitioned Resource Bound: cycles the busiest unit needs per iteration.
  ii     the iteration interval cl6x achieved: one iteration starts every ii cycles.
  unr    how many source iterations one pipelined iteration holds (unrolling), so a sample
         costs ii / unr cycles in the loop's steady state.

ii == rec means the loop waits on a recurrence with units idle: run independent work in the
same loop (fuse passes, or two channels or voices side by side). ii == res means the units are
full: only fewer operations help. A loop cl6x did not pipeline is printed with its reason -- a
call it could not inline, a branch, an unknown alias -- and is the first thing to fix.

The compile uses build_app.py's own TIFLAGS, so the code is the code the package gets; -mw
(--debug_software_pipeline) and -k only add the report and keep the assembly. Needs TI's cl6x,
which tools/env.sh finds under ~/ti; run it in WSL on the build machine.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENAGA = ROOT / "third_party" / "enaga"
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402

FIELDS = {
    "line": re.compile(r"Loop source line\s*:\s*(\d+)"),
    "trip": re.compile(r"Known Minimum Trip Count\s*:\s*(\d+)"),
    "rec": re.compile(r"Loop Carried Dependency Bound\(\^\)\s*:\s*(\d+)"),
    "res": re.compile(r"Partitioned Resource Bound\(\*\)\s*:\s*(\d+)"),
    "unr": re.compile(r"Loop Unroll Multiple\s*:\s*(\d+)x"),
    "ii": re.compile(r"ii = (\d+)\s+Schedule found with (\d+) iterations in parallel"),
    "dq": re.compile(r"Disqualified loop:\s*(.*)"),
}


def compile_one(src, out, defines):
    """cl6x with the package's flags plus the pipeline report; returns the .asm path or None."""
    flags = [*build_app.TIFLAGS, "-mw", "-k", f"--asm_directory={out}", f"--obj_directory={out}",
             f"-I{paths.TI_CGT / 'include'}", f"-I{ROOT / 'dsp'}", f"-I{ROOT / 'core'}",
             *(f"--define={d}" for d in defines)]
    r = subprocess.run([str(paths.TI_CGT / "bin" / "cl6x"), *flags, "-c", str(src)], cwd=out,
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"{src.name}: cl6x failed\n{r.stdout}{r.stderr}", file=sys.stderr)
        return None
    asm = out / (src.name.rsplit(".", 1)[0] + ".asm")
    return asm if asm.exists() else None


def loops(asm):
    """One dict per SOFTWARE PIPELINE INFORMATION block, with the function it sits in."""
    function, block, found = "?", None, []
    for raw in asm.read_text(errors="replace").splitlines():
        if re.match(r"^\s*\.sect\s+\"\.text:(\w+)\"", raw):
            function = re.match(r"^\s*\.sect\s+\"\.text:(\w+)\"", raw).group(1)
        if "SOFTWARE PIPELINE INFORMATION" in raw:
            block = {"function": function}
            found.append(block)
            continue
        if block is None or not raw.startswith(";*"):
            continue
        for key, rx in FIELDS.items():
            m = rx.search(raw)
            if m and key not in block:
                block[key] = m.groups() if key == "ii" else m.group(1)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--enaga", action="store_true", help="also Enaga's runtime/dsp/*.c")
    ap.add_argument("-D", dest="defines", action="append", default=[], metavar="NAME=VALUE")
    ap.add_argument("--keep", type=Path, help="keep the annotated assembly here")
    args = ap.parse_args()

    if not paths.ti_present():
        sys.exit("error: TI's cl6x not found (paths.py looks under ~/ti); this report needs it")

    sources = sorted([*ROOT.glob("core/*.ti.c"), *ROOT.glob("dsp/*.ti.c")])
    if args.enaga:
        sources += sorted((ENAGA / "runtime" / "dsp").glob("*.c"))

    with tempfile.TemporaryDirectory() as tmp:
        out = args.keep or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        print(f"cl6x {paths.TI_CGT.name}, flags: build_app.TIFLAGS"
              + (" -D " + " -D ".join(args.defines) if args.defines else ""))
        print()
        print("| file:line | function | rec | res | ii | unr | cycles/iter | bound | note |")
        print("| --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for src in sources:
            asm = compile_one(src, out, args.defines)
            if asm is None:
                print(f"| {src.name} | | | | | | | | COMPILE FAILED |")
                continue
            for lp in loops(asm):
                where = f"{src.name}:{lp.get('line', '?')}"
                if "dq" in lp:
                    print(f"| {where} | {lp['function']} | | | | | | | NOT PIPELINED: {lp['dq']} |")
                    continue
                ii = lp.get("ii")
                rec, res = lp.get("rec", "?"), lp.get("res", "?")
                unr = int(lp.get("unr", "1"))
                if ii is None:
                    print(f"| {where} | {lp['function']} | {rec} | {res} | | | | | no schedule found |")
                    continue
                n_ii, par = int(ii[0]), ii[1]
                bound = ("recurrence" if rec != "?" and n_ii <= int(rec)
                         else "resources" if res != "?" and n_ii <= int(res) else "neither")
                trip = f"min trip {lp['trip']}, " if "trip" in lp else ""
                print(f"| {where} | {lp['function']} | {rec} | {res} | {n_ii} | {unr} | "
                      f"{n_ii / unr:.1f} | {bound} | {trip}{par} in parallel |")
        if args.keep:
            print(f"\nassembly kept in {out}")


if __name__ == "__main__":
    main()
