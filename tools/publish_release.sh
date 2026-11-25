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
# Run it in WSL, from a clean checkout of main, after the walkthrough in docs/UNIT_CHECKLIST.md
# passed and Roland said go. It refuses at the first thing that is not right and changes nothing
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
#      script's tests pass, and tools/package_release.py builds the zip.
#
# The zip never carries firmware: tools/package_release.py takes only APP.ENA from the card.
#
# Then (5) it commits the filled notes on a branch release/<tag>, tags that commit, pushes both, creates
# the GitHub release (a pre-release, the zip and its .sha256 attached, the notes as its body) and
# opens a PR that brings the dated changelog into main.
#
# --dry-run does 0-4 and stops: it shows the release body and leaves the tree as it was.
# --draft creates the GitHub release as a draft instead, to look at before pressing Publish.
#
# Needs: git, python3, gh (logged in, `gh auth status`), and the device toolchain for check 3.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)

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
[ -z "$(git status --porcelain --untracked-files=no)" ] || die "the tree has uncommitted changes"
git fetch -q origin main --tags
[ "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)" ] \
    || die "HEAD is not origin/main: check out main and pull, then rebuild the card from it"
git rev-parse -q --verify "refs/tags/$TAG" >/dev/null && die "tag $TAG exists already"
git ls-remote --exit-code --tags origin "$TAG" >/dev/null 2>&1 && die "tag $TAG exists on GitHub already"
HAVE_GH=0
if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then
    HAVE_GH=1
    gh release view "$TAG" >/dev/null 2>&1 && die "a GitHub release $TAG exists already"
fi
[ "$DRY" -eq 1 ] || [ "$HAVE_GH" -eq 1 ] || die "gh is missing or not logged in (gh auth login)"
echo "main at $(git rev-parse --short HEAD), $TAG is free"

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
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
if [ "$SKIP_ENAGA" -eq 1 ]; then
    echo "SKIPPED (--skip-enaga-check)"
elif [ "$HAVE_GH" -ne 1 ]; then
    die "checking Enaga's release needs gh (or --skip-enaga-check after opening
    https://github.com/seawaffle/enaga/releases/tag/v$ENAGA_VERSION by hand)"
else
    gh release download "v$ENAGA_VERSION" --repo seawaffle/enaga --dir "$WORK/enaga" \
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
FILES=(CHANGELOG.md "$NOTES")
restore() { git checkout -q -- "${FILES[@]}"; rm -rf "$WORK"; }
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
python3 -B tools/package_release.py "$TAG" --card "$CARD" --out build/release
ZIP="build/release/STRATA-$TAG-smpltrek.zip"
ZIP_SHA=$(cut -d' ' -f1 "$ZIP.sha256")
BODY="build/release/$TAG-notes.md"
sed '1,/^---$/d' "$NOTES" | sed "s/{{ZIP_SHA256}}/$ZIP_SHA/g" > "$BODY"
LEFT=$(grep -oE '\{\{[A-Z_0-9]+\}\}|TODO\(release\)' "$BODY" CHANGELOG.md | sort -u || true)
[ -z "$LEFT" ] || die "still unfilled: $LEFT"

if [ "$DRY" -eq 1 ]; then
    say "dry run: nothing committed, tagged, pushed or created. The release body would be:"
    cat "$BODY"
    echo
    echo "zip: $ZIP  sha256 $ZIP_SHA"
    exit 0
fi

# --- 5. commit, tag, push, release ------------------------------------------------------------
say "5. publish"
BRANCH="release/$TAG"
git checkout -q -b "$BRANCH"
git add "${FILES[@]}"
git commit -q -m "release: $TAG

The changelog dated $DATE and the notes filled from the unit test: $BUILD_LABEL build,
$UNIT_LOAD Zip sha256 $ZIP_SHA."
trap 'rm -rf "$WORK"' EXIT          # the filled notes are committed now
git tag -a "$TAG" -m "STRATA $VERSION"
git push -q origin "$BRANCH" "$TAG"

FLAGS=(--prerelease)
[ "$DRAFT" -eq 1 ] && FLAGS+=(--draft)
gh release create "$TAG" "$ZIP" "$ZIP.sha256" --verify-tag --title "STRATA $VERSION" \
    --notes-file "$BODY" "${FLAGS[@]}"
gh pr create --base main --head "$BRANCH" --title "release: $TAG, the dated changelog" \
    --body "The release commit that $TAG points at: CHANGELOG.md dated and the release notes filled from the unit test. Merge it so main has them. Then update README.md's install section, which still says there is no prebuilt release." || true
git checkout -q main

if [ "$DRAFT" -eq 1 ]; then
    say "DRAFT release $TAG created: open it on GitHub and press Publish"
else
    say "PUBLISHED $TAG"
fi
gh release view "$TAG" --json url -q .url
