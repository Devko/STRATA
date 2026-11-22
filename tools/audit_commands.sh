#!/usr/bin/env bash
# The "feature with no gesture" audit (docs/HANDOFF.md, section 5), as a script.
#
# Every name in app/commands.hpp's `enum class Cmd` must be both POSTED (by app/app.hpp, the
# UI, or app/storage.cpp, the card side) and HANDLED (by app/engine.cpp). A command with a
# handler and no poster is a feature nobody can reach; one with a poster and no handler is a
# key that does nothing. M5 shipped four of the first kind and the sends shipped two that were
# both at once, so this is worth running after every milestone.
#
# Comments and `case` labels are stripped before counting posters. The recipe HANDOFF.md first
# gave counted both: `release_note`, which appears only in a comment in app/app.hpp, looked
# posted, and so did `clear_lock`, which app/storage.cpp only READS in mutates_pattern().
#
# The names below are known and argued where they live; anything else fails.
#   none          the command ring's empty slot, never posted.
#   kit_recall    handled, no caller yet: waits for the kit BROWSER half of F1 + F8.
#   release_note  neither: a held KEYS key has no known duration (see app/app.hpp).
#   clear_lock    handled, no caller: docs/SPEC.md gives no gesture for removing one parameter
#                 lock, so a lock can be turned but never taken off a step. The gesture is a
#                 UI decision for a person; until it is made, this name stays listed.
#
# Usage: tools/audit_commands.sh     exit 0 when only the known names come back.
set -euo pipefail

cd "$(dirname "$0")/.."

KNOWN=" none kit_recall release_note clear_lock "

# Drop // comments, whole-line or trailing, so a mention in prose is not a use.
code() { sed -E 's#//.*$##' "$@"; }

names=$(sed -n '/^enum class Cmd/,/^};/p' app/commands.hpp | sed -E 's#//.*$##' |
        grep -oE '^[[:space:]]*[a-z_]+( = [0-9]+)?,' | sed -E 's/ = [0-9]+//' | tr -d ' ,\t')

unexpected=0
printf '%-18s %6s %6s %6s\n' command ui card engine
for n in $names; do
    # A `case` label is a reader, not a poster: app/storage.cpp switches on some commands too.
    ui=$(code app/app.hpp | grep -E "Cmd::$n\b" | grep -cvE "case Cmd::$n\b" || true)
    card=$(code app/storage.cpp | grep -E "Cmd::$n\b" | grep -cvE "case Cmd::$n\b" || true)
    engine=$(code app/engine.cpp | grep -cE "case Cmd::$n\b" || true)
    if [ "$((ui + card))" -gt 0 ] && [ "$engine" -gt 0 ]; then
        if [[ "$KNOWN" == *" $n "* ]]; then
            printf '%-18s %6s %6s %6s  now wired: drop it from KNOWN\n' "$n" "$ui" "$card" "$engine"
            unexpected=$((unexpected + 1))
        fi
        continue
    fi
    if [[ "$KNOWN" == *" $n "* ]]; then
        printf '%-18s %6s %6s %6s  known\n' "$n" "$ui" "$card" "$engine"
    else
        printf '%-18s %6s %6s %6s  UNEXPECTED\n' "$n" "$ui" "$card" "$engine"
        unexpected=$((unexpected + 1))
    fi
done

# The other direction: a name on the known list that is now fully wired should leave it.
for n in $KNOWN; do
    if ! grep -qx "$n" <<<"$names"; then
        echo "known name $n is no longer in enum class Cmd: drop it from this script"
        unexpected=$((unexpected + 1))
    fi
done

if [ "$unexpected" -ne 0 ]; then
    echo "$unexpected command(s) posted with no handler or handled with no poster"
    exit 1
fi
echo "ok: every command is posted and handled, apart from the known names above"
