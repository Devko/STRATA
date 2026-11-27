#!/usr/bin/env python3
"""Where the guest CPU goes: the emulator's guest PC sampler over a warmed scenario, by function.

    python3 -B tools/profile_guest.py STAGE --keys "22@300000000 21@380000000" \
        --from 900000000 --cycles 600000000 [--period 1009] [--top 40] [--json out.json]

STAGE is the folder tools/build.sh stages ($ENAGA_BUILD/stage/STRATA). Keys and turns are the
scenario's own (`--key` and `--turn` syntax of Enaga's emulate.py, space separated). Sampling
starts at --from, once the scripted presses are done, and runs for --cycles.

The emulator's profiler (smpltrek-re emulator/cpu.py, `profile_guest`) samples the PC of issued
execute packets every --period cycles, jittered. It is not cycle-weighted: a packet that stalls
counts once. So shares here say where the code runs, not exact cycles; docs/BUDGET.md's load
figures stay the measure. PCs are mapped to the app's symbols (app.elf: the app is linked at its
load address) and Enaga's firmware symbols, and summed by function.

Builds through tools/enaga_cached.py's wrappers, so a cached build is reused.
"""
import argparse
import bisect
import importlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import enaga_cached  # noqa: E402


def symbols_of(elf, nm):
    """(address, size, name) of every function symbol in an ELF, sorted by address."""
    out = subprocess.run([nm, "-S", "--defined-only", "-C", str(elf)], capture_output=True, text=True, check=True)
    rows = []
    for line in out.stdout.splitlines():
        if "$C$" in line or " $" in line:   # cl6x's local branch labels: keep the enclosing function
            continue
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[2].lower() == "t":
            rows.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
        elif len(parts) == 3 and parts[1].lower() == "t":
            rows.append((int(parts[0], 16), 0, parts[2]))
    return sorted(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage", type=Path)
    ap.add_argument("--keys", default="", help="KEY@CYCLE ..., as the scenario passes --key")
    ap.add_argument("--turns", default="", help="ENC:DETENTS@CYCLE ..., as the scenario passes --turn")
    ap.add_argument("--from", dest="start", type=int, default=900_000_000)
    ap.add_argument("--cycles", type=int, default=600_000_000)
    ap.add_argument("--period", type=int, default=1009)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args(argv)

    enaga = enaga_cached.ENAGA
    sys.path.insert(0, str(enaga / "tools"))
    paths = importlib.import_module("paths")
    build_app = importlib.import_module("build_app")
    enaga_cached.wrap_build_app(build_app, paths)
    emulate = importlib.import_module("emulate")
    enaga_cached.wrap_firmware(emulate.firmware_build, paths)

    events = []
    for item in args.keys.split():
        key, at = item.split("@")
        events += [(int(at, 0), "key", (int(key, 0), True)), (int(at, 0) + 20_000_000, "key", (int(key, 0), False))]
    for item in args.turns.split():
        what, at = item.split("@")
        enc, det = what.split(":")
        events.append((int(at, 0), "turn", (int(enc), int(det))))
    events.append((args.start, "profile", ()))
    events.sort(key=lambda e: e[0])

    with emulate.Emulation(args.stage) as emu:
        now = 0
        for at, kind, data in events + [(args.start + args.cycles, "end", ())]:
            if at > now:
                result = emu.run(at - now)
                if result["error"]:
                    print(f"error: {result['error']}", file=sys.stderr)
                    return 1
                now = emu.cpu.state()["cycles"]
            if kind == "key":
                emu.key(*data)
            elif kind == "turn":
                emu.turn(*data)
                now = emu.cpu.state()["cycles"]
            elif kind == "profile":
                emu.cpu.profile_guest(args.period)
        prof = emu.cpu.guest_profile()
        peak = emu.word("audio_cycles_peak")
        log = emu.log()

    nm = str(Path(os.environ.get("ENAGA_TOOLCHAIN", Path.home() / "strata-toolchain/tic6x")) / "bin/tic6x-elf-nm")
    app_elf = Path(paths.BUILD) / "apps" / args.stage.resolve().name / "app.elf"
    syms = symbols_of(app_elf, nm) if app_elf.exists() else []
    fw = Path(emu.firmware["elf"])
    syms = sorted(syms + [(a, s, "fw:" + n) for a, s, n in symbols_of(fw, nm)])
    starts = [s[0] for s in syms]

    by_fn, total, unknown = {}, 0, 0
    for row in prof["pcs"]:
        total += row["samples"]
        i = bisect.bisect_right(starts, row["pc"]) - 1
        if i < 0 or (syms[i][1] and row["pc"] >= syms[i][0] + syms[i][1]):
            unknown += row["samples"]
            name = f"?{row['pc']:#x}"
        else:
            name = syms[i][2]
        by_fn[name] = by_fn.get(name, 0) + row["samples"]

    ranked = sorted(by_fn.items(), key=lambda kv: -kv[1])
    print(f"app.elf: {app_elf if app_elf.exists() else 'NOT FOUND'}")
    print(f"samples {total} (dropped {prof['dropped_samples']}), unknown {unknown}; "
          f"audio peak {peak} cycles = {peak / 4960:.2f}%")
    for name, n in ranked[:args.top]:
        print(f"{100.0 * n / max(total, 1):6.2f}%  {n:8d}  {name}")
    if args.json:
        args.json.write_text(json.dumps(dict(total=total, peak=peak, log=log, functions=ranked,
                                              pcs=[(r["pc"], r["samples"]) for r in prof["pcs"]]), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
