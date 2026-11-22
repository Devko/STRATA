#!/usr/bin/env bash
# M4 emulator scenario: queue a pattern change, then save the project.
#
# Like tests/scenario_m3.sh, this stands in for host tests that cannot exist: app/ is the only
# folder that includes Enaga headers (CLAUDE.md), so the storage layer, the command ring and the
# queue between them are not reachable from `make -C host test`. And like M3's, it checks
# EFFECTS rather than the absence of a crash.
#
# Two effects, both visible in Enaga's log ring:
#
#   1. app/app.cpp logs `g<16 trig bits in hex> u<undo depth>` whenever the selected track's
#      current page changes. A switch from A01 (the default beat, kick on steps 1, 5, 9 and 13 =
#      g1111) to A05 (empty: the factory fills A01-A04 only, core/factory.hpp) therefore shows up
#      as a line. That is the
#      pattern change, observed rather than assumed.
#   2. app/storage.cpp logs one line per finished save or load. Without it a scenario could only
#      say the app did not crash while saving, which is the least interesting thing about a save.
#
# **The switch is driven with the transport STOPPED on purpose.** A queued change with the clock
# still has no "end of the current pass" to wait for, so Engine::apply switches at once -- which
# is the non-obvious half of docs/HANDOFF.md's queue protocol and the part most likely to be got
# wrong. Testing it stopped also keeps the scenario short: waiting for a 16-step pass at 130 BPM
# would cost another two seconds of emulated time.
#
# The save is FUNC + HOME, then OK: the FUNC menu opens on SAVE (docs/CONTROLS.md). OK in
# PROJECT queues the slot under the cursor, as OK picks in every Enaga list.
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F6 = 29, F3 = 20, F1 = 19, F4 = 27, OK = 23,
# STEP n = 31 + n. A press is released 20 M cycles later.
#
# The first press must come LATE: Enaga's panel MCU link is not streaming in the first ~200 M
# cycles and a press before that is silently swallowed (tests/scenario_m1.sh).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=1000000000   # 100 M more than before: the save is now two presses, 60 M later

while [ $# -gt 0 ]; do
    case "$1" in
        --cycles) CYCLES="$2"; shift 2 ;;
        *) echo "error: unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ ! -f "$ENAGA_EMULATOR/cpu.py" ]; then
    echo "SKIP: no emulator at $ENAGA_EMULATOR" >&2
    exit 0
fi

strata_require_toolchain
strata_link_emulator_toolchain
tools/build.sh > /dev/null

STAGE="$ENAGA_BUILD/stage/STRATA"
mkdir -p "$ENAGA_BUILD"
LOG="$ENAGA_BUILD/scenario_m4.log"

echo "running the emulator for $CYCLES cycles: play, stop, queue A05, save ..."
python3 -B "$ENAGA_ROOT/tools/emulate.py" "$STAGE" --cycles "$CYCLES" \
    --key 22@300000000 \
    --key 29@420000000 \
    --key 20@500000000 \
    --key 36@560000000 \
    --key 19@640000000 --key 27@645000000 \
    --key 23@700000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

python3 - "$LOG" <<'PY'
import json, re, sys

CYCLES_PER_BLOCK = 496_000

text = open(sys.argv[1]).read()
match = re.search(r'^\{.*?^\}', text, re.S | re.M)
if not match:
    print("  FAIL  no JSON report in the emulator output")
    sys.exit(1)
report = json.loads(match.group(0))

failures = []

def check(ok, label, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {label}{'  ' + detail if detail else ''}")
    if not ok:
        failures.append(label)

log = report.get("log", [])

check(report.get("app_status") == "APP RUNNING", "app is running",
      f'status={report.get("app_status")!r}')
check(not report.get("crash_reason"), "no crash", f'reason={report.get("crash_reason")!r}')
check("strata ready" in log, "init() ran to completion")

# init() now also opens the last project and allocates 85 MiB (the sample region, the loader's
# staging buffers and the beat-repeat ring). A failure there returns false from App::init, which
# Enaga reports as APP INIT FAILED rather than APP RUNNING -- so the check above is also the
# memory check, and the first thing to suspect if it fails.
check(report.get("app_name") == "STRATA", "the app Enaga booted is this one",
      f'name={report.get("app_name")!r}')

check("run int" in log, "the transport started", f"log={log}")
check("stopped" in log, "and stopped again before the switch")

# The pattern change. A01 is the default beat on track 1 (kick on 1, 5, 9, 13 = g1111); A05 is
# past the four the factory fills, so it is empty. With the transport stopped the switch is immediate, so the
# empty grid must appear in the trail.
grids = [line for line in log if re.fullmatch(r"g[0-9A-F]{4} u\d+", line)]
check(len(grids) >= 2, "the grid changed at all", f"grids={grids}")
check(grids and grids[0] == "g1111 u0", "it started on the default beat", f"first={grids[:1]}")
check("g0000 u0" in grids, "queueing A05 switched to an empty pattern", f"grids={grids}")

if grids and "g0000 u0" not in grids:
    print("        the queue did not switch with the transport stopped, which is the one case")
    print("        Engine::apply handles specially -- there is no pass end to wait for")

# The save. One line, from Storage::finish_job.
saved = [line for line in log if line.startswith(("saved", "autosaved", "save failed"))]
check(any(line.startswith(("saved", "autosaved")) for line in saved),
      "the project saved to the card", f"lines={saved}")

# Stopping the transport autosaves (docs/SPEC.md), so that line should be there too.
check("autosaved" in log, "stopping the transport autosaved", f"log={log}")

if any(line.startswith("save failed") for line in saved):
    print("        the card refused the write. If it says NO CARD, emulate.py's 4 MiB FAT image")
    print("        is read-only in this emulator and this part needs the unit instead.")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding before the switch", f"{percent:.2f}% peak")
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here, and this is the GCC-only build -- compare versions, not")
print("      targets. docs/BUDGET.md records cl6x as worth about 2.2x. The unit decides.")

sys.exit(1 if failures else 0)
PY
rc=$?

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
