#!/usr/bin/env python3
"""The scenario harness's own pieces, against fakes (CI has no emulator and no tic6x toolchain):

  tools/enaga_cached.py  wrap_full_log  every log line since boot, not the newest 16
                         wrap_turn      no detent lost when a main-loop pass outlasts a phase
                         wrap_compile   one object per source, cached by its inputs, in parallel
  tools/scenario_pick.py which scenarios a change needs (tools/scenarios.sh --changed)
  tools/menu_rows.py     a menu row's place, read from the enum that orders the menu

    python3 -B tests/test_scenario_tools.py
"""
import os
import sys
import tempfile
import types
from pathlib import Path

STRATA = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(STRATA / "tools"))
import enaga_cached  # noqa: E402
import menu_rows  # noqa: E402
import scenario_pick  # noqa: E402

failures = []


def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)


# --- a fake emulator: Enaga's log ring and its encoder decoder --------------------------------
MS = 372_000
RING = 32
LOG_COUNT, LOG_LINES, LAST_PHASE = 0x1000, 0x2000, 0x3000


class FakeCpu:
    """Guest memory with Enaga's log ring; a main loop that polls the encoders every `pass_ms`
    (firmware/hal/encoders.c's decoder) and logs `per_run` lines on every run() call."""

    def __init__(self, pass_ms=1, per_run=0):
        self.cycles, self.pass_ms, self.per_run = 0, pass_ms, per_run
        self.count, self.lines = 0, [b""] * RING
        self.pins = {}
        self.last_phase, self.direction, self.encoder_count = [3] * 5, [0] * 5, [0] * 5
        self.next_poll = 0

    def log(self, text):
        self.lines[self.count % RING] = text.encode()
        self.count += 1

    def gpio_input(self, bank, pin, value):
        self.pins[(bank, pin)] = int(bool(value))

    def phase(self, encoder):
        (ba, pa), (bb, pb) = ENCODER_PINS[encoder]
        return self.pins.get((ba, pa), 1) | self.pins.get((bb, pb), 1) << 1

    def poll(self):
        for i in range(5):
            now = self.phase(i)
            if now == self.last_phase[i]:
                continue
            if now == 3:
                self.direction[i] = 0
            elif self.last_phase[i] == 3:
                self.direction[i] = -1 if now == 1 else 1 if now == 2 else 0
            elif now == 0 and self.direction[i]:
                self.encoder_count[i] += self.direction[i]
            self.last_phase[i] = now

    def run(self, cycles):
        end = self.cycles + cycles
        while self.next_poll < end:
            self.poll()
            self.next_poll += self.pass_ms * MS
        self.cycles = end
        for _ in range(self.per_run):
            self.log(f"line {self.count}")
        return {"cycles": self.cycles, "status": "budget"}

    def state(self):
        return {"cycles": self.cycles}

    def read(self, address, size):
        if address == LOG_COUNT:
            return self.count.to_bytes(4, "little")
        if address == LOG_LINES:
            return b"".join(line.ljust(40, b"\0")[:40] for line in self.lines)[:size]
        if LAST_PHASE <= address < LAST_PHASE + 5:
            return bytes(self.last_phase[address - LAST_PHASE:address - LAST_PHASE + size])
        raise ValueError(hex(address))


ENCODER_PINS = [((3, 14), (3, 15)), ((3, 12), (3, 10)), ((5, 2), (5, 0)), ((5, 5), (5, 4)), ((5, 7), (5, 8))]


def fake_emulate(cpu_factory):
    """A module shaped like Enaga's tools/emulate.py, as far as the wrappers use it."""
    m = types.ModuleType("emulate")
    m.MS, m.LOG_LINES, m.ENCODER_PINS = MS, RING, ENCODER_PINS

    def turn(emu, encoder, detents, phase_ms=3):         # emulate.py's own: 3 ms a phase
        (ba, pa), (bb, pb) = ENCODER_PINS[encoder]
        for _ in range(abs(detents)):
            for phase in ((2, 0, 1, 3) if detents > 0 else (1, 0, 2, 3)):
                emu.cpu.gpio_input(ba, pa, phase & 1)
                emu.cpu.gpio_input(bb, pb, phase >> 1 & 1)
                emu.run(phase_ms * MS)

    class Emulation:
        def __init__(self, names=None):
            self.cpu = cpu_factory()
            self.names = names if names is not None else \
                {"log_count": LOG_COUNT, "log_lines": LOG_LINES, "last_phase": LAST_PHASE}

        def word(self, name):
            return int.from_bytes(self.cpu.read(self.names[name], 4), "little")

        def run(self, cycles, slice_cycles=1_000_000):   # emulate.py's: in 1 M-cycle slices
            end = self.cpu.cycles + cycles
            while self.cpu.cycles < end:
                self.cpu.run(min(slice_cycles, end - self.cpu.cycles))
            return {"error": None}

        def log(self):                                   # emulate.py's: the newest 16
            count = self.word("log_count")
            raw = self.cpu.read(self.names["log_lines"], RING * 40)
            return [raw[(i % RING) * 40:(i % RING) * 40 + 40].split(b"\0")[0].decode()
                    for i in range(max(0, count - 16), count)]

        def report(self):
            return {"log": self.log()}

        def turn(self, encoder, detents, phase_ms=3):
            m.turn(self, encoder, detents, phase_ms)

    m.turn, m.Emulation = turn, Emulation
    return m


def test_full_log():
    em = fake_emulate(lambda: FakeCpu(per_run=1))
    enaga_cached.wrap_full_log(em)
    emu = em.Emulation()
    emu.cpu.log("ENAGA 0.4.0")
    emu.run(100 * 1_000_000)                              # 100 slices, a line each
    log = emu.report()["log"]
    check(len(log) == 101 and log[0] == "ENAGA 0.4.0" and log[-1] == "line 100",
          "the report's log holds every line since boot", f"{len(log)} lines, first {log[:1]}")
    check(emu.report()["log_lost"] == 0, "no line lost at a line a slice")
    em2 = fake_emulate(lambda: FakeCpu(per_run=40))
    enaga_cached.wrap_full_log(em2)
    emu2 = em2.Emulation()
    emu2.run(1_000_000)
    check(emu2.report()["log_lost"] == 8 and len(emu2.log()) == 32,
          "more lines than the ring in one slice are counted as lost", str(emu2.report()["log_lost"]))


def test_turn():
    slow = lambda: FakeCpu(pass_ms=5)                      # a main-loop pass of 5 ms > 3 ms a phase
    plain = fake_emulate(slow)
    emu = plain.Emulation()
    emu.turn(2, 32)
    check(emu.cpu.encoder_count[2] < 32, "emulate.py's own turn loses detents to a slow main loop",
          f"{emu.cpu.encoder_count[2]} of 32")
    wrapped = fake_emulate(slow)
    enaga_cached.wrap_turn(wrapped)
    emu = wrapped.Emulation()
    emu.turn(2, 32)
    emu.turn(1, -5)
    check(emu.cpu.encoder_count[2] == 32 and emu.cpu.encoder_count[1] == -5,
          "the wrapped turn loses none", f"{emu.cpu.encoder_count[2]}, {emu.cpu.encoder_count[1]}")
    fast = fake_emulate(lambda: FakeCpu(pass_ms=1))
    enaga_cached.wrap_turn(fast)
    emu = fast.Emulation()
    emu.turn(0, 4)
    check(emu.cpu.cycles == 4 * 4 * 3 * MS, "with a quick main loop a detent still takes 12 ms",
          f"{emu.cpu.cycles / MS:.0f} ms")
    nosym = fake_emulate(slow)
    enaga_cached.wrap_turn(nosym)
    emu = nosym.Emulation(names={"log_count": LOG_COUNT, "log_lines": LOG_LINES})
    emu.turn(2, 8)
    check(emu.cpu.cycles == 8 * 4 * 3 * MS, "without a last_phase symbol it is emulate.py's turn")


def test_compile():
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        os.environ["STRATA_ENAGA_CACHE"] = str(d / "cache")
        app = d / "stage"
        app.mkdir()
        (app / "a.cpp").write_text("a1")
        (app / "b.cpp").write_text("b1")
        (app / "loops.ti.c").write_text("t1")
        (app / "x.hpp").write_text("h1")
        runtime = d / "runtime"
        runtime.mkdir()
        (runtime / "runtime.cpp").write_text("rt")
        compiled = []
        tool = d / "g++"
        tool.write_text("compiler")

        def compile_source(src, out, name=None):
            obj = Path(out) / f"{name or Path(src).name}.o"
            compiled.append(Path(src).name)
            obj.write_bytes(b"OBJ " + Path(src).read_bytes())
            return obj

        ba = types.SimpleNamespace(compile_source=compile_source, RUNTIME=runtime, CFLAGS=["-c"],
                                   CXXFLAGS=["-x"], TIFLAGS=["-O3"], GCC=tool, GXX=tool)
        paths = types.SimpleNamespace(ti_present=lambda: False, TI_CGT=None)
        enaga_cached._header_digests.clear()
        precompile = enaga_cached.wrap_compile(ba, paths)
        out = d / "out"
        precompile(app, out)
        check(sorted(compiled) == ["a.cpp", "b.cpp", "loops.ti.c", "runtime.cpp"],
              "the first build compiles every source and the runtime", str(sorted(compiled)))
        compiled.clear()
        for src in ("a.cpp", "b.cpp", "loops.ti.c"):
            ba.compile_source(app / src, out)
        ba.compile_source(runtime / "runtime.cpp", out, "enaga.runtime.cpp")
        check(compiled == [] and (out / "a.cpp.o").read_bytes() == b"OBJ a1",
              "build()'s own loop after it finds every object cached", str(compiled))
        (app / "b.cpp").write_text("b2")
        compiled.clear()
        precompile(app, d / "out2")
        check(compiled == ["b.cpp"] and (d / "out2" / "b.cpp.o").read_bytes() == b"OBJ b2",
              "a changed source recompiles that source alone", str(compiled))
        (app / "x.hpp").write_text("h2")
        enaga_cached._header_digests.clear()
        compiled.clear()
        precompile(app, d / "out3")
        check(len(compiled) == 3, "a changed header recompiles the app's sources", str(sorted(compiled)))
        os.environ["STRATA_ENAGA_CACHE"] = "off"
        compiled.clear()
        precompile(app, d / "out4")
        check(len(compiled) == 4, "STRATA_ENAGA_CACHE=off compiles everything")
        del os.environ["STRATA_ENAGA_CACHE"]


def test_pick():
    every = len(scenario_pick.scenarios())
    check(scenario_pick.pick(["docs/MANUAL.md", "README.md", "web/index.html"]) == {},
          "prose needs no scenario")
    check(scenario_pick.pick(["tools/env.sh"]) is None and scenario_pick.pick(["third_party/enaga"]) is None,
          "the harness or Enaga needs every scenario")
    got = scenario_pick.pick(["core/taptempo.hpp"])
    check(set(got) == {"tempo", *scenario_pick.SMOKE}, "a core file needs the scenarios naming it, and smoke",
          str(sorted(got)))
    got = scenario_pick.pick(["dsp/grain.cpp"])
    check({"grain", *scenario_pick.LOAD, *scenario_pick.SMOKE} <= set(got), "a DSP file adds the load set",
          str(sorted(got)))
    got = scenario_pick.pick(["tests/scenario_osc.sh"])
    check("osc" in got and len(got) < every, "a changed scenario is picked, and not everything")
    check(scenario_pick.stem("dsp/valoops.ti.c") == "va" and scenario_pick.stem("app/ui_trackmenu.hpp") == "trackmenu",
          "stems drop ui_, loops and the suffixes")


def test_menu_rows():
    rows = menu_rows.rows("app/ui_perfmix.hpp", "MenuItem")
    check(rows[0] == "save" and "count" not in rows and "chain" in rows, "the FUNC menu's rows, in order")
    check(menu_rows.up("app/ui_perfmix.hpp", "MenuItem", rows[-1]) == 1, "UP once from the first row is the last")
    check(menu_rows.down("app/ui_trackmenu.hpp", "TrackRow", "generate") == 0, "DOWN 0 is the first row")


for test in (test_full_log, test_turn, test_compile, test_pick, test_menu_rows):
    print(test.__name__)
    test()
print("all pass" if not failures else f"FAILED: {failures}")
sys.exit(1 if failures else 0)
