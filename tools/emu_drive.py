"""Drive the emulator from Python, for scenarios that hold a key longer than --key does.

emulate.py's --key holds a key for 20 M cycles (54 ms). GRID's track menu (app/ui_trackmenu.hpp)
opens on OK held 500 ms, so a scenario that reaches it runs the emulator here instead, as
tests/scenario_gen.sh does inline, and collects the log as it goes (the report keeps only its
last 16 lines). The firmware and app builds go through tools/enaga_cached.py like every other
scenario's (tools/check_rules.sh, rule 4).

    import emu_drive
    d = emu_drive.Drive(stage)
    d.go(300 * emu_drive.M); d.tap(emu_drive.PLAY)
    d.track_menu(); d.tap(emu_drive.UP)
    ...
    d.common_checks(min_blocks=1500); sys.exit(d.finish())

Run from the repository root (the scenarios cd there).
"""
import importlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "third_party" / "enaga" / "tools"))
import enaga_cached  # noqa: E402
import menu_rows  # noqa: E402

paths = importlib.import_module("paths")
build_app = importlib.import_module("build_app")
enaga_cached.wrap_build_app(build_app, paths)
emulate = importlib.import_module("emulate")
enaga_cached.wrap_emulate(emulate, paths)

M = 1_000_000
MS = emulate.MS
CYCLES_PER_BLOCK = 496_000

# Keys (third_party/enaga/include/enaga/abi.h).
UP, RIGHT, F1, F3, PLAY, OK, LEFT, DOWN, F4, CLR = 17, 18, 19, 20, 22, 23, 24, 25, 27, 30


def track_menu_up(row):
    """UP presses from GENERATE, round the bottom, to the track menu's `row` (a TrackRow name):
    read from app/ui_trackmenu.hpp, so a row added there does not break the walk."""
    return menu_rows.up("app/ui_trackmenu.hpp", "TrackRow", row)


def step_key(n):
    """STEP n, 1-16."""
    return 31 + n


class Drive:
    def __init__(self, stage):
        self.emu = emulate.Emulation(stage)
        self.seen = 0
        self.log = []
        self.failures = []
        self.cycle = 0

    def collect(self):
        count = self.emu.word("log_count")
        lines = self.emu.cpu.read(self.emu.names["log_lines"], emulate.LOG_LINES * 40)
        first = max(self.seen, count - emulate.LOG_LINES)
        for i in range(first, count):
            at = (i % emulate.LOG_LINES) * 40
            self.log.append(lines[at:at + 40].split(b"\0")[0].decode("ascii", "replace"))
        self.seen = count

    def go(self, cycles):
        # In slices, so a pass's worth of log lines never outruns the ring.
        left = int(cycles)
        while left > 0:
            step = min(left, 100 * M)
            result = self.emu.run(step)
            if result["error"]:
                print(f"  FAIL emulator stopped: {result['error']}")
                sys.exit(1)
            self.collect()
            self.cycle += step
            left -= step

    def go_to(self, cycle):
        """On to an absolute cycle; nothing if it is already past."""
        if cycle > self.cycle:
            self.go(cycle - self.cycle)

    def tap(self, key, hold=12 * M):
        self.emu.key(key, True)
        self.go(hold)
        self.emu.key(key, False)
        self.go(14 * M)

    def chord(self, first, second):
        """`first` held, `second` tapped 5 M cycles later: FUNC + HOME is chord(F1, F4)."""
        self.emu.key(first, True)
        self.go(5 * M)
        self.emu.key(second, True)
        self.go(12 * M)
        self.emu.key(second, False)
        self.emu.key(first, False)
        self.go(14 * M)

    def track_menu(self):
        """GRID's track menu on the selected track: OK held 700 ms (it opens at 500)."""
        self.emu.key(OK, True)
        self.go(700 * MS)
        self.emu.key(OK, False)
        self.go(14 * M)

    def check(self, ok, label, detail=""):
        print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
        if not ok:
            self.failures.append(label)

    def index(self, line, after=-1):
        for i in range(after + 1, len(self.log)):
            if self.log[i] == line:
                return i
        return -1

    def common_checks(self, min_blocks, sounding=True):
        """Running, no crash, no overrun, the load inside the target. Returns the peak in %."""
        report = self.emu.report()
        self.check(report.get("app_status") == "APP RUNNING", "app is running",
                   f'status={report.get("app_status")!r}')
        self.check(not report.get("crash_reason"), "no crash",
                   f'reason={report.get("crash_reason")!r}')
        blocks = report.get("audio_blocks", 0)
        self.check(blocks > min_blocks, "audio ran throughout", f"blocks={blocks}")
        self.check(report.get("audio_overruns", -1) == 0, "no audio overruns",
                   f'overruns={report.get("audio_overruns")}')
        peak = report.get("audio_cycles_peak", 0)
        percent = 100.0 * peak / CYCLES_PER_BLOCK
        if sounding:
            self.check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
        self.check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")
        print()
        print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, "
              f"{blocks} blocks)")
        return percent

    def finish(self):
        print("log:", self.log)
        self.emu.close()
        return 1 if self.failures else 0
