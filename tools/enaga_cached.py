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

For emulate it also moves the emulator's decode cache next to this one (use_local_decode_cache).

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
import threading
from concurrent.futures import ThreadPoolExecutor
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


_HEADER_SUFFIXES = {".h", ".hpp", ".inc"}
_header_digests = {}


def _headers_digest(folder):
    """Every header under `folder`, by name and bytes; once per folder per process."""
    folder = Path(folder)
    if folder not in _header_digests:
        h = hashlib.sha256()
        files = sorted(q for q in folder.rglob("*") if q.is_file() and q.suffix in _HEADER_SUFFIXES)
        for q in files:
            h.update(f"{q.relative_to(folder).as_posix()}\n".encode())
            h.update(hashlib.sha256(q.read_bytes()).digest())
        _header_digests[folder] = h.hexdigest()
    return _header_digests[folder]


def wrap_compile(build_app, paths):
    """One object at a time, cached by its own inputs, so a change recompiles only what it touches.

    build_app.build() compiles every source of the app, one after the other, on every call, and
    the whole-app cache above misses on any change at all: a one-line edit cost the full serial
    build. compile_source() is wrapped here to look each object up in <cache>/obj first, keyed by
    the source's bytes, every header beside it and in Enaga's include/ (any header change
    recompiles everything: coarse, never stale), the compiler that builds it and its flags with
    the include path taken out (the objects carry no path: no -g, no __FILE__), so worktrees with
    the same sources share objects. build() then compiles the misses side by side before Enaga's
    own loop runs, which finds every object in the cache (see wrap_build_app).
    """
    if not all(hasattr(build_app, n) for n in ("compile_source", "RUNTIME", "CFLAGS", "CXXFLAGS")):
        return None                                 # a build_app of another shape: whole builds only
    real = build_app.compile_source
    include = str(ENAGA / "include")

    def key_of(src, ti):
        h = hashlib.sha256(b"strata object cache v1\n")
        if ti:
            h.update(repr(build_app.TIFLAGS).encode())
            _hash_tool(h, Path(paths.TI_CGT) / "bin" / "cl6x")
            h.update(_headers_digest(Path(paths.TI_CGT) / "include").encode())
        else:
            flags = build_app.CFLAGS if src.suffix == ".c" else build_app.CXXFLAGS
            h.update(repr([f.replace(include, "<enaga>/include") for f in map(str, flags)]).encode())
            _hash_tool(h, build_app.GCC if src.suffix == ".c" else build_app.GXX)
        h.update(f"{src.suffix} {src.name.endswith('.ti.c')}\n".encode())
        h.update(hashlib.sha256(src.read_bytes()).digest())
        h.update(_headers_digest(src.parent).encode())
        h.update(_headers_digest(ENAGA / "include").encode())
        return h.hexdigest()

    def compile_source(src, out, name=None):
        root = cache_root()
        if root is None:
            return real(src, out, name)
        src, out = Path(src), Path(out)
        obj = out / f"{name or src.name}.o"
        ti = src.name.endswith(".ti.c") and paths.ti_present()
        key = key_of(src, ti)
        entry = root / "obj" / key[:2] / f"{key}.o"
        if entry.exists():
            out.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(entry, obj)
            return obj
        result = real(src, out, name)
        entry.parent.mkdir(parents=True, exist_ok=True)
        tmp = entry.with_name(f".{key}.{os.getpid()}.{threading.get_ident()}")
        shutil.copyfile(obj, tmp)
        os.replace(tmp, entry)
        return result

    build_app.compile_source = compile_source

    def precompile(app_dir, out):
        """Every object build() is about to ask for, compiled side by side (STRATA_BUILD_JOBS, by
        default every core up to 8). Errors are raised as build() would raise them."""
        app_dir, out = Path(app_dir).resolve(), Path(out).resolve()
        work = [(src, None) for src in sorted(app_dir.glob("*.cpp")) + sorted(app_dir.glob("*.c"))]
        work.append((build_app.RUNTIME / "runtime.cpp", "enaga.runtime.cpp"))
        out.mkdir(parents=True, exist_ok=True)
        jobs = int(os.environ.get("STRATA_BUILD_JOBS", "0")) or min(os.cpu_count() or 2, 8)
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            for future in [pool.submit(compile_source, src, out, name) for src, name in work]:
                future.result()

    return precompile


def wrap_build_app(build_app, paths):
    real = build_app.build
    precompile = wrap_compile(build_app, paths) if os.environ.get("STRATA_OBJ_CACHE", "on") != "off" else None

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
        if precompile is not None:
            precompile(app_dir, out)
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


def use_local_decode_cache():
    """Keep the emulator's decode cache beside ours, off /mnt/d.

    The emulator decodes each 4 KiB page of guest code once and keeps the result in
    <its repo>/work/emulator/decode-cache, which for STRATA is the clone under /mnt/d. A run reads
    a few hundred of those files, and on the 9p mount that cost about 9 s of a 30 s scenario_m3
    (2026-11-27). The cache is keyed by the page bytes and the decoder's sources, so moving it
    changes nothing but where the files are. SMPLTREK_DECODE_CACHE set by hand still wins.
    """
    root = cache_root()
    if root is not None:
        os.environ.setdefault("SMPLTREK_DECODE_CACHE", str(root / "emulator-decode"))


def wrap_blocks(emulate):
    """Compiled blocks: the emulator turns hot guest code into native code (emulator/blocks.py).

    Every 50 M guest cycles, Emulation.run() stops, compiles the blocks that got hot since the
    last round and installs them, then goes on. The libraries are kept in the cache by their
    source, so a later run mostly loads them, and an app change recompiles only what changed.
    Measured on the full scenario set at -j 4 with cl6x (2026-11-27): 789 s without, 663 s on
    the first run after an app change, 305 s after that, and all 46 load figures identical.

    STRATA_EMU_BLOCKS=off turns it off, to rule it out when an emulator result looks wrong. The
    blocks folder grows by about 100 MB per app version; delete <cache>/emulator to reclaim it.
    """
    if os.environ.get("STRATA_EMU_BLOCKS", "on") == "off":
        return
    if not hasattr(emulate, "Emulation"):          # an emulator without the Emulation class
        return
    root = cache_root() or Path.home() / "strata-build" / "cache"
    real_init = emulate.Emulation.__init__
    real_run = emulate.Emulation.run
    every = int(os.environ.get("STRATA_EMU_BLOCK_ROUND", "50000000"))
    block_work = root / "emulator"

    def init(self, *a, **k):
        real_init(self, *a, **k)
        self._next_round = None                    # None: this emulator has no compiled blocks
        if hasattr(self.cpu, "blocks"):
            self.cpu.blocks(True, heat=True)
            self._known = set()
            self._next_round = every

    def run(self, cycles, slice_cycles=1_000_000):
        if self._next_round is None:
            return real_run(self, cycles, slice_cycles)
        end = self.cpu.state()["cycles"] + cycles
        total = 0.0
        last = None
        while True:
            now = self.cpu.state()["cycles"]
            if now >= end:
                break
            step = min(end, max(self._next_round, now + 1)) - now
            last = real_run(self, step, slice_cycles)
            total += last["host_seconds"]
            if last["error"]:
                break
            if self.cpu.state()["cycles"] >= self._next_round:
                import blocks as emu_blocks
                sources = emu_blocks.collect(self.cpu, known=self._known)
                if sources:
                    emu_blocks.install(self.cpu, emu_blocks.build(block_work, sources), sources)
                    self._known.update(sources)
                self._next_round = self.cpu.state()["cycles"] + every
        if last is None:
            return real_run(self, 0, slice_cycles)
        return {**last, "host_seconds": total}

    emulate.Emulation.__init__ = init
    emulate.Emulation.run = run


def wrap_full_log(emulate):
    """The report's "log" holds every line Enaga logged since boot, not only the newest 16.

    Enaga keeps its last 32 lines in a ring (log_lines, log_count counts every line ever logged;
    firmware/include/kernel.h), and emulate.py's report gives the newest 16. A scenario checking
    for a line therefore failed whenever anything logged after it pushed it out: boot itself
    logging two more lines (ENAGA 0.4.0, CACHE) broke eight scenarios at once (2026-11-28). This
    reads the ring after every slice the emulator runs (1 M cycles from emulate.py's own run(),
    under 3 ms), so a line is lost only if more than 32 are logged inside one slice; those are
    counted in the report's "log_lost". Lines keep their order, and a scenario that checked the
    last 16 still finds them at the end of the list.
    """
    E = getattr(emulate, "Emulation", None)
    if E is None or not all(hasattr(E, n) for n in ("word", "log", "report")):
        return                                      # an emulate.py of another shape
    ring = getattr(emulate, "LOG_LINES", 32)
    real_init, real_report = E.__init__, E.report

    def collect(self):
        count = self.word("log_count")
        if count <= self._log_seen:
            return
        first = max(self._log_seen, count - ring)
        self._log_lost += first - self._log_seen
        raw = self.cpu.read(self.names["log_lines"], ring * 40)
        for i in range(first, count):
            at = (i % ring) * 40
            self._log_all.append(raw[at:at + 40].split(b"\0")[0].decode("ascii", "replace"))
        self._log_seen = count

    def init(self, *a, **k):
        real_init(self, *a, **k)
        self._log_all, self._log_seen, self._log_lost = [], 0, 0
        cpu_run = self.cpu.run

        def run(cycles):
            state = cpu_run(cycles)
            collect(self)
            return state

        self.cpu.run = run

    def log(self):
        collect(self)
        return list(self._log_all)

    def report(self):
        return {**real_report(self), "log_lost": self._log_lost}

    E.__init__, E.log, E.report = init, log, report


def wrap_turn(emulate):
    """Encoder turns that never lose a detent.

    emulate.py holds each of a detent's four pin phases for a fixed 3 ms. Enaga reads the pins
    once per main-loop pass (firmware/hal/encoders.c: the first phase after rest sets the
    direction, arriving at phase 0 counts), so a pass longer than 3 ms -- a busy screen in the
    emulator -- skips a phase and the detent is lost (scenario_energy lost two of 32,
    2026-11-28). Here each phase is held at least `phase_ms`, as before, and then until the
    firmware's decoder has seen it (its last_phase), up to 100 ms a phase. Where the firmware has
    no last_phase symbol it falls back to emulate.py's own turn.
    """
    if not hasattr(emulate, "turn"):
        return
    real_turn = emulate.turn
    pins = emulate.ENCODER_PINS
    ms = emulate.MS

    def turn(emu, encoder, detents, phase_ms=3):
        at = getattr(emu, "names", {}).get("last_phase")
        if at is None:
            return real_turn(emu, encoder, detents, phase_ms)
        (ba, pa), (bb, pb) = pins[encoder]
        sequence = (2, 0, 1, 3) if detents > 0 else (1, 0, 2, 3)
        for _ in range(abs(detents)):
            for phase in sequence:
                emu.cpu.gpio_input(ba, pa, phase & 1)
                emu.cpu.gpio_input(bb, pb, phase >> 1 & 1)
                emu.run(phase_ms * ms)
                waited = phase_ms
                while emu.cpu.read(at + encoder, 1)[0] != phase and waited < 100:
                    emu.run(ms)
                    waited += 1

    emulate.turn = turn


def wrap_emulate(emulate, paths):
    """Everything this file adds to Enaga's emulate module, for a script that drives the emulator
    from Python (tools/emu_drive.py, tests/scenario_gen.sh): the cached firmware, the local decode
    cache, compiled blocks, the full log and lossless turns. Call before making an Emulation."""
    wrap_firmware(emulate.firmware_build, paths)
    use_local_decode_cache()
    wrap_blocks(emulate)
    wrap_full_log(emulate)
    wrap_turn(emulate)


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
        wrap_emulate(module, paths)
    sys.argv = [str(ENAGA / "tools" / f"{tool}.py"), *args]
    return module.main(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
