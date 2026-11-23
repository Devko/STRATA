#!/usr/bin/env python3
"""Run one of Enaga's tools with its app and firmware builds cached by content.

    python3 -B tools/enaga_cached.py build_app STAGE -o OUT --name STRATA     (tools/build.sh)
    python3 -B tools/enaga_cached.py emulate STAGE --cycles N --key K@C ...   (tests/scenario_*.sh)

Why: Enaga's build_app.build() compiles every source of the app from scratch on every call, and
emulate.py calls it again (and builds the firmware too) on every run. A full tools/scenarios.sh
therefore compiled STRATA 28 times for 14 scenarios, all from the same sources.

How: this imports Enaga's own modules from $ENAGA_ROOT/tools -- third_party/enaga is never
modified (CLAUDE.md) -- and wraps two functions before calling the tool's main():

  build_app.build(app_dir, out, name, app_version)
      keyed by the bytes of every file in app_dir, Enaga's include/ and runtime/ trees,
      build_app.py itself, the toolchain and cl6x in use (path, size and mtime of the compilers)
      and the arguments. A hit returns the stored package and copies app.elf into `out`.
  firmware_build.build(out)      (emulate.py only)
      keyed by the bytes of firmware/, loader/, the tools it imports and the toolchain.

A different source, header, flag, compiler or ENAGA_TI_CGT is a different key, so a hit is
always the exact bytes a fresh build would have produced from the same inputs. The cache lives in
$STRATA_ENAGA_CACHE (default ~/strata-build/cache), outside every per-scenario $ENAGA_BUILD, so
parallel scenarios (tools/scenarios.sh -j) share it. Entries are written to a temporary folder and
renamed into place, so two processes building the same key at once are safe. Delete the folder
to start over; STRATA_ENAGA_CACHE=off turns the cache off.
"""
import hashlib
import importlib
import json
import os
import pickle
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
STRATA = HERE.parent
ENAGA = Path(os.environ.get("ENAGA_ROOT", STRATA / "third_party" / "enaga")).resolve()

_SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".s", ".S", ".ld", ".txt", ".py", ".inc"}


def cache_root():
    chosen = os.environ.get("STRATA_ENAGA_CACHE", "")
    if chosen == "off":
        return None
    return Path(chosen) if chosen else Path.home() / "strata-build" / "cache"


def _hash_tree(h, root, label):
    """Every source-like file under root, in a stable order, by relative name and bytes."""
    root = Path(root)
    if not root.exists():
        h.update(f"{label}:missing\n".encode())
        return
    files = [root] if root.is_file() else sorted(p for p in root.rglob("*") if p.is_file())
    for p in files:
        if p.is_file() and (p.suffix in _SOURCE_SUFFIXES or root.is_file()):
            rel = p.name if root.is_file() else p.relative_to(root).as_posix()
            # __pycache__, and build_app's own output when it writes into the app folder
            # (app_dir/build/ when the app is outside the Enaga tree, as STRATA's stage is).
            if "__pycache__" in rel or rel.startswith("build/"):
                continue
            h.update(f"{label}/{rel}\n".encode())
            h.update(hashlib.sha256(p.read_bytes()).digest())


def _hash_tool(h, path):
    """A compiler by where it is, how big it is and when it was installed."""
    p = Path(path)
    if p.exists():
        st = p.stat()
        h.update(f"tool {p} {st.st_size} {st.st_mtime_ns}\n".encode())
    else:
        h.update(f"tool {p} missing\n".encode())


def _toolchain(h, paths):
    h.update(f"toolchain {paths.TOOLCHAIN}\n".encode())
    for name in ("gcc", "g++", "as", "ld", "objcopy"):
        _hash_tool(h, paths.tool(name))
    ti = paths.ti_present() if hasattr(paths, "ti_present") else False
    h.update(f"ti {paths.TI_CGT} {ti}\n".encode())
    if ti:
        _hash_tool(h, Path(paths.TI_CGT) / "bin" / "cl6x")


def _store(kind, key, write):
    """write(folder) fills a new entry; it lands at <cache>/<kind>/<key> atomically."""
    root = cache_root()
    final = root / kind / key
    final.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f".{key[:12]}.", dir=final.parent))
    try:
        write(tmp)
        try:
            os.rename(tmp, final)
        except OSError:
            pass          # another process stored the same key first: same inputs, same bytes
    finally:
        if tmp.exists():
            shutil.rmtree(tmp, ignore_errors=True)
    return final


def _say(message):
    if os.environ.get("STRATA_ENAGA_CACHE_QUIET") != "1":
        print(f"enaga_cached: {message}", file=sys.stderr)


def wrap_build_app(build_app, paths):
    real = build_app.build

    def build(app_dir, out, name=None, app_version=1):
        if cache_root() is None:
            return real(app_dir, out, name, app_version)
        h = hashlib.sha256(b"strata build_app cache v1\n")
        h.update(f"args {name} {app_version}\n".encode())
        _hash_tree(h, app_dir, "app")
        _hash_tree(h, ENAGA / "include", "include")
        _hash_tree(h, ENAGA / "runtime", "runtime")
        _hash_tree(h, ENAGA / "tools" / "build_app.py", "build_app")
        _hash_tree(h, ENAGA / "tools" / "paths.py", "paths")
        _toolchain(h, paths)
        key = h.hexdigest()
        entry = cache_root() / "app" / key
        out = Path(out)
        if (entry / "meta.json").exists():
            meta = json.loads((entry / "meta.json").read_text())
            out.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(entry / "app.elf", out / "app.elf")
            _say(f"app build cached ({key[:12]})")
            return {**meta, "package": (entry / "package.bin").read_bytes(), "elf": out / "app.elf"}
        result = real(app_dir, out, name, app_version)

        def write(folder):
            (folder / "package.bin").write_bytes(result["package"])
            shutil.copyfile(result["elf"], folder / "app.elf")
            meta = {k: v for k, v in result.items() if k not in ("package", "elf")}
            (folder / "meta.json").write_text(json.dumps(meta))

        _store("app", key, write)
        _say(f"app built and cached ({key[:12]})")
        return result

    build_app.build = build


def wrap_firmware(firmware_build, paths):
    real = firmware_build.build

    def build(out=None):
        out = Path(out) if out is not None else firmware_build.OUT
        if cache_root() is None:
            return real(out)
        h = hashlib.sha256(b"strata firmware cache v1\n")
        _hash_tree(h, ENAGA / "firmware", "firmware")
        _hash_tree(h, ENAGA / "loader", "loader")
        _hash_tree(h, ENAGA / "include", "include")
        for tool in ("paths.py", "sysimage.py"):
            _hash_tree(h, ENAGA / "tools" / tool, tool)
        _toolchain(h, paths)
        key = h.hexdigest()
        entry = cache_root() / "firmware" / key
        if (entry / "result.pickle").exists():
            _say(f"firmware build cached ({key[:12]})")
            return pickle.loads((entry / "result.pickle").read_bytes())
        result = real(out)

        # The result names files in `out`, which belongs to one $ENAGA_BUILD; the cached copy
        # names the same files inside the entry, so it stays valid when that folder is gone.
        def write(folder):
            shutil.copytree(out, folder / "out", dirs_exist_ok=True)

            def rebase(v):
                if isinstance(v, Path) and v.is_relative_to(out):
                    return entry / "out" / v.relative_to(out)
                if isinstance(v, dict):
                    return {k: rebase(x) for k, x in v.items()}
                return v

            (folder / "result.pickle").write_bytes(pickle.dumps(rebase(result)))

        _store("firmware", key, write)
        _say(f"firmware built and cached ({key[:12]})")
        return result

    firmware_build.build = build


def main(argv):
    if len(argv) < 1 or argv[0] not in ("build_app", "emulate"):
        print(__doc__, file=sys.stderr)
        return 2
    tool, args = argv[0], argv[1:]
    sys.path.insert(0, str(ENAGA / "tools"))
    paths = importlib.import_module("paths")
    build_app = importlib.import_module("build_app")
    wrap_build_app(build_app, paths)
    module = build_app
    if tool == "emulate":
        module = importlib.import_module("emulate")
        wrap_firmware(module.firmware_build, paths)
    sys.argv = [str(ENAGA / "tools" / f"{tool}.py"), *args]
    return module.main(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
