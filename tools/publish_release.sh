#!/usr/bin/env bash
# Publishes a STRATA release in one step, from the card that passed the unit test.
#
#   tools/publish_release.sh v0.1.0 --build cl6x --ti-licence-read --max-a01 23.5 --max-a04 31.2
#   tools/publish_release.sh v0.1.0 --build gcc --max-a01 38.0 --max-a04 47.5 --dry-run
#   tools/publish_release.sh v0.1.0 --build cl6x --ti-licence-read --unit-load "Load stayed ..."
#
# --unit-load replaces the two MAX figures with a sentence of your own, for when the clean A01
# and A04 readings were not taken. It goes into the notes as written, so end it with a full stop.
#
# Run it in WSL, from a clean checkout of main (on Roland's PC: the release thread's own
# worktree, never D:\DEV\STRATA), after the walkthrough in docs/UNIT_CHECKLIST.md passed and
# Roland said go. It refuses at the first thing that is not right and changes nothing
# until every check has passed:
#
#   0. a cl6x build needs --ti-licence-read (docs/release/RELEASING.md);
#   1. the tree is clean and HEAD is origin/main; the tag and the release do not exist yet;
#   2. the card's APP.ENA is byte for byte a fresh build of HEAD with the same compiler
#      (tools/build.sh), so the release is the code it says it is;
#   3. Enaga's own release of the pinned version (X.Y.Z read from the pin) is on GitHub with its
#      SmplTrek zip, since the release links it instead of shipping firmware; and when the card
#      still has the SmplTrek.bin the unit was updated from, that file is byte for byte the one in
#      Enaga's zip, so the unit test ran on Enaga's own build;
#   4. the {{FIELDS}} in CHANGELOG.md and docs/release/v<version>.md are filled, the packaging
#      script's tests pass, and tools/package_release.py builds the zip; then
#      tools/release_docs.py pins the release in the card builder's web/public/compat.json and
#      writes it into README.md's status line and the zip names in README.md and docs/INSTALL.md.
#
# The zip never carries firmware: tools/package_release.py takes only APP.ENA from the card.
#
# Then (5) it commits the filled notes and those files on a branch release/<tag>, tags that commit,
# pushes both, creates the GitHub release (a pre-release, the zip and its .sha256 attached, the
# notes as its body) and opens a PR that brings them into main. The card builder offers the
# release from GitHub at once; deploying web/ after that PR merges pins it there too (Roland).
#
# --dry-run does 0-4 and stops: it shows the release body and the changes to compat.json, README.md
# and docs/INSTALL.md, and leaves the tree as it was.
# --draft creates the GitHub release as a draft instead, to look at before pressing Publish.
#
# Needs: git, python3, gh (logged in, `gh auth status`), and the device toolchain for check 2.
#
# On Roland's PC the worktree is made by Windows git, which WSL's git cannot read, and gh and the
# GitHub credentials exist only on Windows. So in a worktree whose .git names a Windows path, the
# script runs Windows' own git.exe and gh.exe (WSL runs Windows programs from /mnt/c), and builds
# with WSL as before. STRATA_GIT and STRATA_GH override the choice.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

# git and gh: Windows' own in a Windows-made worktree seen from WSL (tools/env.sh).
# shellcheck source=tools/env.sh
source tools/env.sh
GIT="$STRATA_GIT"
GH="$STRATA_GH"

die() { echo "publish_release: $*" >&2; exit 1; }
say() { echo "== $*"; }

TAG="${1:-}"
[ -n "$TAG" ] && [ "${TAG#-}" = "$TAG" ] || die "usage: tools/publish_release.sh vX.Y.Z --build cl6x|gcc (--max-a01 N --max-a04 N | --unit-load TEXT) [--ti-licence-read] [--card DIR] [--date YYYY-MM-DD] [--draft] [--dry-run]"
shift
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the tag should look like v0.1.0, not $TAG"
VERSION="${TAG#v}"

BUILD="" MAX_A01="" MAX_A04="" UNIT_LOAD="" TI_READ=0 CARD="$ROOT/build/card" DATE=$(date +%F) DRAFT=0 DRY=0
SKIP_BUILD=0 SKIP_ENAGA=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build)             BUILD="$2"; shift 2 ;;
        --max-a01)           MAX_A01="$2"; shift 2 ;;
        --max-a04)           MAX_A04="$2"; shift 2 ;;
        --unit-load)         UNIT_LOAD="$2"; shift 2 ;;
        --ti-licence-read)   TI_READ=1; shift ;;
        --card)              CARD="$2"; shift 2 ;;
        --date)              DATE="$2"; shift 2 ;;
        --draft)             DRAFT=1; shift ;;
        --dry-run)           DRY=1; shift ;;
        --skip-build-check)  SKIP_BUILD=1; shift ;;   # only when the card was built elsewhere on purpose
        --skip-enaga-check)  SKIP_ENAGA=1; shift ;;   # only when gh cannot reach GitHub; check by hand
        *) die "unknown argument: $1" ;;
    esac
done

case "$BUILD" in
    cl6x) BUILD_LABEL="cl6x" ;;
    gcc)  BUILD_LABEL="GCC-only" ;;
    *)    die "--build cl6x or --build gcc: say which compiler built the card's APP.ENA" ;;
esac
if [ -n "$UNIT_LOAD" ]; then
    [ -z "$MAX_A01$MAX_A04" ] || die "--unit-load or the two MAX figures, not both"
    [[ "$UNIT_LOAD" == *. ]] || die "--unit-load: a sentence, ending with a full stop"
else
    [[ "$MAX_A01" =~ ^[0-9]+(\.[0-9])?$ ]] || die "--max-a01: the MAX reading on A01, e.g. 23.5 (or --unit-load)"
    [[ "$MAX_A04" =~ ^[0-9]+(\.[0-9])?$ ]] || die "--max-a04: the MAX reading on A04, e.g. 31.2 (or --unit-load)"
    UNIT_LOAD="MAX, the highest audio load since PLAY, read $MAX_A01% on A01 GROOVE and $MAX_A04% on A04 PEAK."
fi
[[ "$DATE" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || die "--date YYYY-MM-DD"
if [ "$BUILD" = cl6x ] && [ "$TI_READ" -ne 1 ]; then
    die "a cl6x build ships only after TI's licence text in ~/ti/ti-cgt-c6000_* was read for any
    condition on the compiler's output (docs/release/RELEASING.md). Pass --ti-licence-read once
    that is done and recorded in docs/BUDGET.md, or release a GCC-only build."
fi

NOTES="docs/release/$TAG.md"
[ -f "$NOTES" ] || die "no $NOTES"
[ -f "$CARD/ENAGA/APPS/STRATA/APP.ENA" ] || die "no ENAGA/APPS/STRATA/APP.ENA in $CARD"

# --- 1. the repository ---------------------------------------------------------------------
say "1. repository"
[ -z "$("$GIT" status --porcelain --untracked-files=no)" ] || die "the tree has uncommitted changes"
"$GIT" fetch -q origin main --tags
[ "$("$GIT" rev-parse HEAD)" = "$("$GIT" rev-parse origin/main)" ] \
    || die "HEAD is not origin/main: check out main and pull, then rebuild the card from it"
"$GIT" rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG exists already"
"$GIT" ls-remote --exit-code --tags origin "$TAG" >/dev/null 2>&1 && die "tag $TAG exists on GitHub already"
HAVE_GH=0
if command -v "$GH" >/dev/null && "$GH" auth status >/dev/null 2>&1; then
    HAVE_GH=1
    "$GH" release view "$TAG" >/dev/null 2>&1 && die "a GitHub release $TAG exists already"
fi
[ "$DRY" -eq 1 ] || [ "$HAVE_GH" -eq 1 ] || die "$GH is missing or not logged in ($GH auth login)"
# Where to come back to after step 5: the branch or the commit the worktree is on now.
ORIG=$("$GIT" symbolic-ref -q --short HEAD || "$GIT" rev-parse HEAD)
echo "main at $("$GIT" rev-parse --short HEAD), $TAG is free"

# --- 2. the app is a build of HEAD -------------------------------------------------------------
say "2. the card's APP.ENA is a $BUILD_LABEL build of HEAD"
if [ "$SKIP_BUILD" -eq 1 ]; then
    echo "SKIPPED (--skip-build-check)"
else
    # shellcheck source=tools/env.sh
    source tools/env.sh
    if [ "$BUILD" = gcc ]; then export ENAGA_TI_CGT=none; fi
    tools/build.sh --name STRATA >/dev/null
    cmp -s "$ENAGA_BUILD/STRATA/APP.ENA" "$CARD/ENAGA/APPS/STRATA/APP.ENA" \
        || die "the card's APP.ENA differs from a $BUILD_LABEL build of HEAD: rebuild the card from
    main (tools/build.sh) and test that, or check --build"
    echo "identical"
fi

# --- 3. Enaga's release, which the zip links instead of shipping -------------------------------
ENAGA_VERSION=$(python3 -B -c 'import sys; sys.path.insert(0, "tools"); import package_release as p; print(".".join(map(str, p.pinned_firmware_version())))')
say "3. Enaga's v$ENAGA_VERSION release is up (the zip links it, and does not carry firmware)"
mkdir -p build
WORK=$(mktemp -d build/publish.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
if [ "$SKIP_ENAGA" -eq 1 ]; then
    echo "SKIPPED (--skip-enaga-check)"
elif [ "$HAVE_GH" -ne 1 ]; then
    die "checking Enaga's release needs gh (or --skip-enaga-check after opening
    https://github.com/seawaffle/enaga/releases/tag/v$ENAGA_VERSION by hand)"
else
    "$GH" release download "v$ENAGA_VERSION" --repo seawaffle/enaga --dir "$WORK/enaga" \
        --pattern "enaga-v$ENAGA_VERSION-smpltrek.zip" >/dev/null \
        || die "Enaga's v$ENAGA_VERSION release has no enaga-v$ENAGA_VERSION-smpltrek.zip: the README.txt
    would send users to a release they cannot install from"
    echo "https://github.com/seawaffle/enaga/releases/tag/v$ENAGA_VERSION has enaga-v$ENAGA_VERSION-smpltrek.zip"
    if [ -f "$CARD/SmplTrek.bin" ]; then
        WANT=$(sha256sum "$CARD/SmplTrek.bin" | cut -d' ' -f1)
        FOUND=$(python3 -B - "$WORK/enaga" "$WANT" <<'PY'
import hashlib, sys, zipfile
from pathlib import Path
root, want = Path(sys.argv[1]), sys.argv[2]
for f in sorted(root.iterdir()):
    if zipfile.is_zipfile(f):
        with zipfile.ZipFile(f) as z:
            for n in z.namelist():
                if n.endswith("SmplTrek.bin") and hashlib.sha256(z.read(n)).hexdigest() == want:
                    print(f"{f.name}:{n}")
                    sys.exit(0)
PY
)
        [ -n "$FOUND" ] || die "the card's SmplTrek.bin ($WANT) is not the file in Enaga's v$ENAGA_VERSION
    release, so the unit test did not run on Enaga's own build. Install the release's
    SmplTrek.bin and test again."
        echo "the card's SmplTrek.bin, which stays out of the zip, is identical to $FOUND"
    fi
fi

# --- 4. fill the notes, test, package ----------------------------------------------------------
say "4. notes, tests, zip"
FILES=(CHANGELOG.md "$NOTES" README.md docs/INSTALL.md web/public/compat.json)
restore() { "$GIT" checkout -q -- "${FILES[@]}"; rm -rf "$WORK"; }
trap restore EXIT
fill() {   # fill <file> KEY=VALUE ...: replace {{KEY}} with VALUE
    python3 -B - "$@" <<'PY'
import sys
path, pairs = sys.argv[1], sys.argv[2:]
text = open(path, encoding="utf-8").read()
for pair in pairs:
    key, value = pair.split("=", 1)
    text = text.replace("{{" + key + "}}", value)
open(path, "w", encoding="utf-8", newline="\n").write(text)
PY
}
for f in "${FILES[@]}"; do
    fill "$f" "RELEASE_DATE=$DATE" "BUILD=$BUILD_LABEL" "UNIT_LOAD=$UNIT_LOAD"
done
python3 -B tests/test_package_release.py >/dev/null 2>&1 || die "tests/test_package_release.py failed"
python3 -B tests/test_release_docs.py >/dev/null 2>&1 || die "tests/test_release_docs.py failed"
python3 -B tools/package_release.py "$TAG" --card "$CARD" --out build/release
ZIP="build/release/STRATA-$TAG-smpltrek.zip"
ZIP_SHA=$(cut -d' ' -f1 "$ZIP.sha256")
BODY="build/release/$TAG-notes.md"
sed '1,/^---$/d' "$NOTES" | sed "s/{{ZIP_SHA256}}/$ZIP_SHA/g" > "$BODY"
# The card builder (web/, strata.einblick.dev) reads which Enaga this release needs from here.
printf '\n<!-- strata-card: {"enaga":"%s"} -->\n' "$ENAGA_VERSION" >> "$BODY"

# The card builder's pinned list (web/public/compat.json), README.md's status line and the zip's
# name in README.md and docs/INSTALL.md. An Enaga the list does not pin yet also needs its zip's
# SHA-256 (downloaded in step 3) and its LICENSE's at the tag (what the card builder checks).
ENAGA_ARGS=()
if ! python3 -B -c 'import json, sys; sys.exit(sys.argv[1] not in json.load(open("web/public/compat.json"))["enaga"])' "$ENAGA_VERSION"; then
    EZIP="$WORK/enaga/enaga-v$ENAGA_VERSION-smpltrek.zip"
    [ -f "$EZIP" ] || die "Enaga $ENAGA_VERSION is new to web/public/compat.json, which needs its zip's SHA-256:
    run without --skip-enaga-check"
    "$GH" api "repos/seawaffle/enaga/contents/LICENSE?ref=v$ENAGA_VERSION" \
        -H "Accept: application/vnd.github.raw" > "$WORK/ENAGA-LICENSE" \
        || die "cannot fetch Enaga's LICENSE at v$ENAGA_VERSION for web/public/compat.json"
    [ -s "$WORK/ENAGA-LICENSE" ] || die "Enaga's LICENSE at v$ENAGA_VERSION came back empty"
    ENAGA_ARGS=(--enaga-zip-sha256 "$(sha256sum "$EZIP" | cut -d' ' -f1)"
                --enaga-license-sha256 "$(sha256sum "$WORK/ENAGA-LICENSE" | cut -d' ' -f1)")
fi
python3 -B tools/release_docs.py "$TAG" --date "$DATE" --zip-sha256 "$ZIP_SHA" \
    --enaga "$ENAGA_VERSION" "${ENAGA_ARGS[@]}" || die "tools/release_docs.py refused (above)"
if command -v node > /dev/null; then
    (cd web && node --test test/*.test.mjs > /dev/null 2>&1) || die "the card builder's tests fail
    with the new compat.json (cd web && npm test)"
else
    echo "no node here: the card builder's tests run in CI on the release PR"
fi
LEFT=$(grep -oE '\{\{[A-Z_0-9]+\}\}|TODO\(release\)' "$BODY" CHANGELOG.md | sort -u || true)
[ -z "$LEFT" ] || die "still unfilled: $LEFT"

if [ "$DRY" -eq 1 ]; then
    say "dry run: nothing committed, tagged, pushed or created. The release body would be:"
    cat "$BODY"
    echo
    say "and these changes, committed with the notes:"
    "$GIT" --no-pager diff -- README.md docs/INSTALL.md web/public/compat.json
    echo "zip: $ZIP  sha256 $ZIP_SHA"
    exit 0
fi

# --- 5. commit, tag, push, release ------------------------------------------------------------
say "5. publish"
BRANCH="release/$TAG"
"$GIT" checkout -q -b "$BRANCH"
"$GIT" add "${FILES[@]}"
"$GIT" commit -q -m "release: $TAG

The changelog dated $DATE and the notes filled from the unit test: $BUILD_LABEL build,
$UNIT_LOAD Zip sha256 $ZIP_SHA.

The release pinned in the card builder's web/public/compat.json, and named in README.md's
status and the zip names in README.md and docs/INSTALL.md (tools/release_docs.py)."
trap 'rm -rf "$WORK"' EXIT          # the filled notes are committed now
"$GIT" tag -a "$TAG" -m "STRATA $VERSION"
"$GIT" push -q origin "$BRANCH" "$TAG"

FLAGS=(--prerelease)
[ "$DRAFT" -eq 1 ] && FLAGS+=(--draft)
"$GH" release create "$TAG" "$ZIP" "$ZIP.sha256" --verify-tag --title "STRATA $VERSION" \
    --notes-file "$BODY" "${FLAGS[@]}"
"$GH" pr create --base main --head "$BRANCH" --title "release: $TAG, the dated changelog" \
    --body "The release commit that $TAG points at: CHANGELOG.md dated and the release notes filled from the unit test, the release pinned in the card builder's web/public/compat.json, and README.md's status and the zip names in README.md and docs/INSTALL.md moved to $TAG. Merge it so main has them, deploy the card builder (\`cd web && npx wrangler pages deploy --branch main\`), then follow docs/release/RELEASING.md, \"After\"." || true
# Back where it started: main itself is usually checked out in another worktree, where a
# checkout of it here would fail.
"$GIT" checkout -q "$ORIG"

if [ "$DRAFT" -eq 1 ]; then
    say "DRAFT release $TAG created: open it on GitHub and press Publish"
else
    say "PUBLISHED $TAG"
fi
"$GH" release view "$TAG" --json url -q .url
