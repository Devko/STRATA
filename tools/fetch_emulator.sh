#!/usr/bin/env bash
# Clone the emulator (seawaffle/smpltrek-re) into third_party/smpltrek-re and check out the
# commit tools/env.sh pins, STRATA_EMULATOR_COMMIT.
#
# The emulator is a plain clone, not a submodule (Enaga's tools take it by ENAGA_EMULATOR), so
# nothing else holds it still: a bare `git clone` takes whatever upstream main is that day, and
# scenario figures would then move without any STRATA change. Idempotent; an existing clone is
# fetched and moved to the pin, unless it has local changes, which are left alone.
set -euo pipefail

cd "$(dirname "$0")/.."
# shellcheck source=tools/env.sh
source tools/env.sh

DIR="third_party/smpltrek-re"

if [ ! -d "$DIR/.git" ]; then
    git clone --quiet "$STRATA_EMULATOR_URL" "$DIR"
fi
if [ -n "$(git -C "$DIR" status --porcelain --untracked-files=no)" ]; then
    echo "error: $DIR has local changes; not moving it" >&2
    exit 1
fi
if ! git -C "$DIR" cat-file -e "$STRATA_EMULATOR_COMMIT^{commit}" 2>/dev/null; then
    git -C "$DIR" fetch --quiet origin
fi
git -C "$DIR" checkout --quiet --detach "$STRATA_EMULATOR_COMMIT"
echo "emulator at $(git -C "$DIR" log -1 --format='%h %ad %s' --date=short)"
