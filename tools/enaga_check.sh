#!/usr/bin/env bash
# Is STRATA where it says it is on Enaga and the emulator, and has upstream moved?
#
#   tools/enaga_check.sh              local checks (lists upstream's tags if the clone has none)
#   tools/enaga_check.sh --upstream   also fetch both upstreams and report what is new
#
# Local checks (exit 1 if any fails):
#   - third_party/enaga is checked out at the commit this repo records for it (the gitlink, as
#     staged),
#     not one a stray `git checkout` inside the submodule left behind;
#   - that commit is a release tag (vX.Y.Z). The pin has to match the Enaga firmware on the unit,
#     and Enaga's loader only checks ENAGA_ABI_VERSION, so headers and firmware can drift apart
#     with nothing failing. Releases are what get installed, so releases are what we pin;
#   - CLAUDE.md, docs/ENAGA_API.md and THIRD_PARTY_NOTICES.md name that tag and its short hash;
#   - the emulator clone, if present, is at STRATA_EMULATOR_COMMIT (tools/env.sh).
#
# --upstream adds a report: newer Enaga release tags, and the commits since the pin that touch
# what an app is built from (include/, runtime/, loader/, build_app.py, paths.py). It exits 3
# when there is a newer release or such a commit, so a scheduled run can tell "nothing to do"
# from "look at this". Nothing is changed either way: moving the pin is a decision, and its
# procedure is docs/HANDOFF.md, "Moving the Enaga pin".
set -uo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

UPSTREAM=0
case "${1:-}" in
    "") ;;
    --upstream) UPSTREAM=1 ;;
    *) echo "usage: tools/enaga_check.sh [--upstream]" >&2; exit 2 ;;
esac

SUB="third_party/enaga"
EMU="third_party/smpltrek-re"
API_PATHS=(include runtime loader tools/build_app.py tools/paths.py)
DOCS=(CLAUDE.md docs/ENAGA_API.md THIRD_PARTY_NOTICES.md)
bad=0
fail() { echo "FAIL  $*"; bad=1; }
ok()   { echo "ok    $*"; }

# --- Enaga -----------------------------------------------------------------
# The index, not HEAD: a staged bump (step 1 of the procedure) is checked before it is committed.
pin=$(git ls-files -s -- "$SUB" | awk '{print $2}')
if [ -z "$pin" ]; then
    echo "FAIL  no $SUB in the index" ; exit 1
fi
if ! git -C "$SUB" rev-parse --verify -q HEAD >/dev/null 2>&1; then
    echo "FAIL  $SUB is not checked out: git submodule update --init --recursive"; exit 1
fi
short=$(git -C "$SUB" rev-parse --short=7 "$pin")
have=$(git -C "$SUB" rev-parse HEAD)
if [ "$have" = "$pin" ]; then
    ok "enaga checked out at the pin $short"
else
    fail "enaga is at $(git -C "$SUB" rev-parse --short=7 HEAD), the pin is $short: git submodule update"
fi

tag=$(git -C "$SUB" tag --points-at "$pin" -l 'v*' | sort -V | tail -1)
if [ -z "$tag" ]; then
    # A shallow clone (CI's --depth=1 submodule) holds only the tags its clone happened to
    # follow: since Enaga v0.3.0 that is v0.3.0 alone, not the pin's v0.2.0, so "fetch the
    # tags if there are none" never fetched them (PR #137). Ask upstream which tags point at
    # the pin instead: ls-remote downloads no objects. The http.*extraheader resets drop the STRATA token that
    # actions/checkout leaves in the submodule's config, which is not for this repository.
    url=$(git -C "$SUB" remote get-url origin)
    if remote=$(git -C "$SUB" -c http.extraheader= -c http.https://github.com/.extraheader= \
            ls-remote --tags "$url" 'v*'); then
        tag=$(awk -v p="$pin" '$1 == p { t = $2; sub("^refs/tags/", "", t); sub("\\^\\{\\}$", "", t); print t }' \
            <<<"$remote" | sort -uV | tail -1)
    else
        echo "--    could not list Enaga's tags at $url"
    fi
fi
if [ -n "$tag" ]; then
    ok "the pin is release $tag"
else
    fail "the pin $short is not a release tag; pin a vX.Y.Z that matches the unit's firmware"
    tag="$short"
fi

abi=$(sed -n 's/^#define ENAGA_ABI_VERSION[[:space:]]*\([0-9]*\)u\{0,1\}.*/\1/p' "$SUB/include/enaga/abi.h")
ok "ENAGA_ABI_VERSION $abi"

for doc in "${DOCS[@]}"; do
    if grep -q -- "$tag" "$doc" && grep -q -- "$short" "$doc"; then
        ok "$doc names $tag ($short)"
    else
        fail "$doc does not name the pin $tag ($short)"
    fi
done

# --- emulator --------------------------------------------------------------
if [ -d "$EMU/.git" ]; then
    emu=$(git -C "$EMU" rev-parse HEAD)
    if [ "$emu" = "$STRATA_EMULATOR_COMMIT" ]; then
        ok "emulator at the pin ${STRATA_EMULATOR_COMMIT:0:7}"
    else
        fail "emulator is at ${emu:0:7}, the pin is ${STRATA_EMULATOR_COMMIT:0:7}: tools/fetch_emulator.sh"
    fi
else
    echo "--    no emulator clone (tools/fetch_emulator.sh); not checked"
fi

[ "$UPSTREAM" -eq 1 ] || exit "$bad"

# --- upstream --------------------------------------------------------------
echo
news=0
if ! git -C "$SUB" fetch --quiet --tags origin; then
    echo "FAIL  could not fetch Enaga upstream"; exit 1
fi
head=$(git -C "$SUB" rev-parse origin/HEAD 2>/dev/null || git -C "$SUB" rev-parse origin/main)
behind=$(git -C "$SUB" rev-list --count "$pin..$head")
echo "enaga upstream: $(git -C "$SUB" rev-parse --short=7 "$head"), $behind commit(s) past the pin"

newer=$(git -C "$SUB" tag -l 'v*' --contains "$pin" | grep -vx -- "$tag" | sort -V)
if [ -n "$newer" ]; then
    news=1
    echo "newer releases:"
    printf '  %s\n' $newer
fi

touching=$(git -C "$SUB" log --format='  %h %ad %s' --date=short "$pin..$head" -- "${API_PATHS[@]}")
if [ -n "$touching" ]; then
    news=1
    echo "commits since the pin that touch ${API_PATHS[*]}:"
    echo "$touching"
    echo "headers changed:"
    git -C "$SUB" diff --stat=100 "$pin" "$head" -- include | sed 's/^/  /'
fi

if [ -d "$EMU/.git" ] && git -C "$EMU" fetch --quiet origin; then
    ehead=$(git -C "$EMU" rev-parse origin/HEAD 2>/dev/null || git -C "$EMU" rev-parse origin/main)
    echo "emulator upstream: ${ehead:0:7}, $(git -C "$EMU" rev-list --count "$STRATA_EMULATOR_COMMIT..$ehead") commit(s) past the pin"
fi

if [ "$bad" -ne 0 ]; then exit 1; fi
if [ "$news" -ne 0 ]; then
    echo; echo "upstream has moved: see docs/HANDOFF.md, \"Moving the Enaga pin\""
    exit 3
fi
echo "nothing new upstream"
