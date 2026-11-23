#!/usr/bin/env python3
"""tools/enaga_cached.py against a stub Enaga tree: a hit returns what the build returned, and any
change to an input (a source, a header, an Enaga file, the compiler, cl6x on or off) misses.

The real build needs the tic6x toolchain, which CI does not have, so the stub's build_app.build
and firmware build count their calls and return what the real ones return. Run by CI's repo checks
and tools/ci_local.sh:

    python3 -B tests/test_enaga_cached.py
"""
import os
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path

STRATA = Path(__file__).resolve().parents[1]
WRAPPER = STRATA / "tools" / "enaga_cached.py"

PATHS_PY = """
import os
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path(os.environ["ENAGA_TOOLCHAIN"])
BUILD = Path(os.environ["ENAGA_BUILD"])
_ti = os.environ.get("ENAGA_TI_CGT", "none")
TI_CGT = None if _ti == "none" else Path(_ti)
def tool(name): return TOOLCHAIN / "bin" / f"tic6x-elf-{name}"
def ti_present(): return TI_CGT is not None and (TI_CGT / "bin" / "cl6x").exists()
"""

BUILD_APP_PY = """
import argparse, os
from pathlib import Path
import paths
COUNT = Path(os.environ["STUB_COUNT"])
def build(app_dir, out, name=None, app_version=1):
    COUNT.write_text(str(int(COUNT.read_text() or 0) + 1) if COUNT.exists() else "1")
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    body = b"".join(p.read_bytes() for p in sorted(Path(app_dir).glob("*.cpp")))
    (out / "app.elf").write_bytes(b"ELF" + body)
    return {"package": b"PKG" + body + str(paths.ti_present()).encode(), "elf": out / "app.elf",
            "payload_size": len(body), "bss_size": 4, "entry": 8, "ti_sources": []}
def main(argv=None):
    ap = argparse.ArgumentParser(); ap.add_argument("app_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path); ap.add_argument("--name")
    ap.add_argument("--app-version", type=int, default=1)
    a = ap.parse_args(argv)
    b = build(a.app_dir, a.app_dir / "build", a.name, a.app_version)
    a.output.write_bytes(b["package"]); return 0
"""

FIRMWARE_PY = """
import os
from pathlib import Path
import paths
OUT = paths.BUILD / "firmware"
COUNT = Path(os.environ["STUB_FW_COUNT"])
def build(out=OUT):
    COUNT.write_text(str(int(COUNT.read_text() or 0) + 1) if COUNT.exists() else "1")
    out.mkdir(parents=True, exist_ok=True)
    (out / "enaga.elf").write_bytes(b"FW")
    return {"elf": out / "enaga.elf", "symbols": {"_start": 1}, "images": {"smpltrek": out / "enaga.elf"}}
"""

EMULATE_PY = """
import importlib.util, json, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_app, paths
_spec = importlib.util.spec_from_file_location("enaga_firmware_build", HERE.parent / "firmware" / "build.py")
firmware_build = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(firmware_build)
def main(argv=None):
    fw = firmware_build.build()
    app = build_app.build(Path(argv[0]), paths.BUILD / "apps" / "X", "STRATA")
    print(json.dumps({"fw_elf": Path(fw["elf"]).read_bytes().decode(), "pkg": app["package"].decode(),
                      "elf_exists": Path(app["elf"]).exists()}))
    return 0
"""


def main():
    failures = []

    def check(ok, label):
        print(f"  {'ok  ' if ok else 'FAIL'} {label}")
        if not ok:
            failures.append(label)

    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        enaga = t / "enaga"
        for rel, text in {"tools/paths.py": PATHS_PY, "tools/build_app.py": BUILD_APP_PY,
                          "tools/emulate.py": EMULATE_PY, "tools/sysimage.py": "",
                          "firmware/build.py": FIRMWARE_PY, "firmware/kernel/a.c": "int a;",
                          "loader/loader.c": "", "include/enaga/app.hpp": "// v1",
                          "runtime/runtime.cpp": ""}.items():
            (enaga / rel).parent.mkdir(parents=True, exist_ok=True)
            (enaga / rel).write_text(textwrap.dedent(text))
        chain = t / "tc"
        (chain / "bin").mkdir(parents=True)
        for tool in ("gcc", "g++", "as", "ld", "objcopy"):
            (chain / "bin" / f"tic6x-elf-{tool}").write_text("x")
        ti = t / "ti"
        (ti / "bin").mkdir(parents=True)
        (ti / "bin" / "cl6x").write_text("x")
        stage = t / "stage"
        stage.mkdir()
        (stage / "a.cpp").write_text("int x;")
        (stage / "a.hpp").write_text("// h")
        env = {**os.environ, "ENAGA_ROOT": str(enaga), "ENAGA_TOOLCHAIN": str(chain),
               "ENAGA_BUILD": str(t / "build1"), "ENAGA_TI_CGT": "none",
               "STRATA_ENAGA_CACHE": str(t / "cache"), "STRATA_ENAGA_CACHE_QUIET": "1",
               "STUB_COUNT": str(t / "count"), "STUB_FW_COUNT": str(t / "fwcount")}

        def builds():
            return int((t / "count").read_text()) if (t / "count").exists() else 0

        def run(*args, **extra):
            r = subprocess.run([sys.executable, "-B", str(WRAPPER), *args], env={**env, **extra},
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout, r.stderr)
            return r

        out = t / "APP.ENA"
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        first = out.read_bytes()
        check(builds() == 1, "the first build compiles")
        out.unlink()
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        check(builds() == 1 and out.read_bytes() == first, "the same inputs hit, with the same bytes")

        (stage / "build").mkdir(exist_ok=True)
        (stage / "build" / "junk.txt").write_text("output, not input")
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        check(builds() == 1, "build_app's own output folder is not an input")

        (stage / "a.hpp").write_text("// h2")
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        check(builds() == 2, "a changed header misses")

        (enaga / "include" / "enaga" / "app.hpp").write_text("// v2")
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        check(builds() == 3, "a changed Enaga header misses")

        run("build_app", str(stage), "-o", str(out), "--name", "STRATA", ENAGA_TI_CGT=str(ti))
        check(builds() == 4 and out.read_bytes().endswith(b"True"), "cl6x on misses, and is used")

        (chain / "bin" / "tic6x-elf-g++").write_text("a newer compiler")
        run("build_app", str(stage), "-o", str(out), "--name", "STRATA")
        check(builds() == 5, "a changed compiler misses")

        run("build_app", str(stage), "-o", str(out), "--name", "OTHER")
        check(builds() == 6, "another name misses")

        run("build_app", str(stage), "-o", str(out), "--name", "STRATA", STRATA_ENAGA_CACHE="off")
        check(builds() == 7, "STRATA_ENAGA_CACHE=off always builds")

        # emulate: the app hits the cache tools/build.sh filled, the firmware is built once, and a
        # second $ENAGA_BUILD (a parallel scenario) reuses both.
        r1 = run("emulate", str(stage))
        r2 = run("emulate", str(stage), ENAGA_BUILD=str(t / "build2"))
        fw = int((t / "fwcount").read_text())
        check(builds() == 7, "emulate reuses the app tools/build.sh built")
        check(fw == 1, "the firmware is built once for two build folders")
        check(r1.stdout == r2.stdout and '"fw_elf": "FW"' in r2.stdout and
              '"elf_exists": true' in r2.stdout, "a cached firmware result points at real files")

    print(f"{'all pass' if not failures else str(len(failures)) + ' failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
