#!/usr/bin/env bash
# Project browser emulator scenario: save the project, SAVE AS a second one, then RENAME the
# first (push B) and DELETE it (push C, OK twice) -- all on keys, which is all the emulator can
# press (the knob pushes are keys: PUSH_B = 0, PUSH_C = 2).
#
# Observable through Enaga's log: app/storage.cpp logs `saved`, `renamed` and `deleted` (or
# `rename failed <why>`, `delete failed <why>`), and app/app.hpp `scr PROJECT PROJECTS` when the
# page shows. It proves the rename's copy and the delete's listing run on the C674x against a
# FAT image, and that the RENAME entry and the delete question draw with the real font.
#
# The sequence, and the names it makes. The project starts as DEFAULT:
#   FUNC + HOME, OK             save DEFAULT
#   PROJECT, screen key x4      the PROJECTS page (DEFAULT, the open one, under the cursor)
#   FUNC + DOWN, UP, OK         SAVE AS: the first letter D -> E, so EEFAULT, now the open one
#   UP                          the cursor onto DEFAULT (the list is sorted: DEFAULT, EEFAULT)
#   push B, DOWN, OK            RENAME DEFAULT -> CEFAULT
#   UP                          the cursor onto CEFAULT, once the list is read again
#   push C, OK, OK              DELETE CEFAULT
#
# Keys (third_party/enaga/include/enaga/abi.h): F5 = 22, F1 = 19, F4 (HOME) = 27, F3 (PROJECT)
# = 20, SELECT_RIGHT = 9, UP = 17, DOWN = 25, OK = 23, PUSH_B = 0, PUSH_C = 2. A press is released
# 20 M cycles later. The two delete OKs are 40 M cycles apart, inside the 2 s (744 M cycles) a
# second press has. Each card job gets 400 M cycles (about a second) before the next key, which
# is far more than eleven files take. The first press comes late: the panel link is not streaming in the first
# ~200 M cycles.
#
# The card is 8 MiB, not emulate.py's default 4: APP.ENA (1.69 MB with cl6x) plus three copies of
# a project (about 0.8 MB each: the save, SAVE AS and the rename's copy) filled 4 MiB, and the
# rename failed CARD FULL with cl6x only, whose image is the larger (2026-11-28).
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

CYCLES=2500000000

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
LOG="$ENAGA_BUILD/scenario_projects.log"

echo "running the emulator for $CYCLES cycles: play, save, SAVE AS, rename, delete ..."
python3 -B tools/enaga_cached.py emulate "$STAGE" --cycles "$CYCLES" --card-mib 8 \
    --key 22@300000000 \
    --key 19@380000000 --key 27@385000000 \
    --key 23@460000000 \
    --key 20@700000000 \
    --key 9@740000000 --key 9@770000000 --key 9@800000000 --key 9@830000000 \
    --key 19@900000000 --key 25@905000000 \
    --key 17@960000000 \
    --key 23@1000000000 \
    --key 17@1400000000 \
    --key 0@1440000000 \
    --key 25@1480000000 \
    --key 23@1520000000 \
    --key 17@1950000000 \
    --key 2@1990000000 \
    --key 23@2030000000 \
    --key 23@2070000000 \
    > "$LOG" 2>&1 || {
    echo "FAIL: emulate.py exited non-zero" >&2
    tail -30 "$LOG" >&2
    exit 1
}

rc=0
python3 - "$LOG" <<'PY' || rc=$?
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
# Enaga's log ring holds sixteen lines and this run writes more (every listing logs one), so the
# early lines may be gone: only the last steps are required.
check("renamed" in log, "push B, DOWN, OK renamed DEFAULT", f"log={log}")
check("deleted" in log, "push C, OK twice deleted it", f"log={log}")
if any(line.startswith(("rename failed", "delete failed", "save failed")) for line in log):
    print("        a card job failed; the reason is on the line. NO CARD or FULL means the")
    print("        emulator's FAT image, not the browser.")

blocks = report.get("audio_blocks", 0)
check(blocks > 1000, "audio ran throughout", f"blocks={blocks}")
check(report.get("audio_overruns", -1) == 0, "no audio overruns",
      f'overruns={report.get("audio_overruns")}')

peak = report.get("audio_cycles_peak", 0)
percent = 100.0 * peak / CYCLES_PER_BLOCK
check(percent > 5.0, "the beat was sounding", f"{percent:.2f}% peak")
check(percent < 60.0, "load is inside the target", f"{percent:.2f}% peak")

print()
print(f"emulator peak load: {percent:.2f}%  ({peak} of {CYCLES_PER_BLOCK} cycles, {blocks} blocks)")
print("note: no cache model here. The copy and the delete run in the main loop, a file a pass,")
print("      and must not cost the audio a block: the overrun check is the one that matters.")

sys.exit(1 if failures else 0)
PY

echo
if [ "$rc" -ne 0 ]; then
    echo "SCENARIO FAILED; full log: $LOG" >&2
    exit 1
fi
echo "SCENARIO OK  (log: $LOG)"
