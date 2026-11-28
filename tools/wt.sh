#!/usr/bin/env bash
# Per-thread worktrees on Roland's PC: make one, list them, remove one safely. Run from WSL.
#
#   tools/wt.sh new <name> [<branch>]   D:\DEV\STRATA-wt\<name> on <branch> (default: <name>),
#                                       with Enaga checked out and $HOME/strata-build-<name> made
#   tools/wt.sh list                    every worktree there: branch, clean or not, pushed or not
#   tools/wt.sh rm <name> [--keep-branch] [--drop-ignored]
#                                       remove it, its build folder and its local branch
#
# Several threads share D:\DEV\STRATA and none may switch its branch, so each works in a worktree
# of its own with its own ENAGA_BUILD (CLAUDE.md, "Running commands"). The worktree is made by
# Windows' git.exe, so that Windows-side git, gh and Claude Code can read it; WSL's git cannot
# (tools/env.sh). Inside D:\DEV\STRATA-wt\<name>, tools/env.sh defaults ENAGA_BUILD to
# $HOME/strata-build-<name>, so builds there never share a folder with another thread.
#
# `new` takes <branch> from origin when it exists there (tracking it), else starts it from
# origin/main. `rm` refuses, and changes nothing, when the worktree:
#   - has uncommitted or untracked changes (Enaga's submodule included),
#   - has a HEAD that no origin/* branch contains (commits that were never pushed),
#   - holds ignored files other than build output and caches (build/, __pycache__, *.pyc,
#     node_modules/, .wrangler/): a nested .claude/worktrees/ checkout, an emulator clone, notes.
#     --drop-ignored removes them anyway, after they were looked at.
# The local branch goes too, unless --keep-branch: its tip is on origin by then, so nothing is lost.
# Nothing here ever touches D:\DEV\STRATA's own checkout, another worktree, or a remote branch.
#
# Environment (defaults suit Roland's PC; tests/test_wt.sh sets them to a scratch folder):
#   STRATA_WT_MAIN   the main checkout       (default: the one this script belongs to)
#   STRATA_WT_ROOT   where worktrees live    (default: <main>/../STRATA-wt)
#   STRATA_WT_GIT    the git to run          (default: git.exe on WSL when the checkout is on a
#                                             Windows drive, else git)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=env.sh
. "$here/env.sh"

die() { echo "error: $*" >&2; exit 1; }
usage() { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) die "run this from WSL: build folders live in WSL's \$HOME, not Windows'" ;;
esac

MAIN="${STRATA_WT_MAIN:-$(strata_main_checkout)}"
MAIN="${MAIN:-$STRATA_ROOT}"
[ -d "$MAIN/.git" ] || die "no main checkout at $MAIN (its .git is not a folder)"
MAIN="$(cd "$MAIN" && pwd)"
WT="${STRATA_WT_ROOT:-$(dirname "$MAIN")/STRATA-wt}"

if [ -n "${STRATA_WT_GIT:-}" ]; then
    GIT="$STRATA_WT_GIT"
elif case "$MAIN" in /mnt/[a-z]/*) true ;; *) false ;; esac &&
     grep -qi microsoft /proc/version 2>/dev/null && command -v git.exe > /dev/null; then
    GIT=git.exe
else
    GIT=git
fi

# A path as $GIT wants it: Windows form for git.exe. The path may not exist yet (`new`), so its
# parent is converted and the last part appended.
gp() {
    if [ "$GIT" = git.exe ]; then
        printf '%s\\%s' "$(wslpath -w "$(dirname "$1")")" "$(basename "$1")"
    else
        printf '%s' "$1"
    fi
}
# A path as git prints it (D:/DEV/... from git.exe), as this shell sees it.
sp() {
    local p="${1%$'\r'}"
    case "$p" in
        [A-Za-z]:[/\\]*) p="${p//\\//}"; p="/mnt/$(printf '%s' "${p:0:1}" | tr 'A-Z' 'a-z')/${p:3}" ;;
    esac
    printf '%s' "$p"
}
# git in a given tree, with git.exe's CRs stripped.
g() { local dir="$1"; shift; "$GIT" -C "$(gp "$dir")" "$@" | tr -d '\r'; }

check_name() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "bad name '$1': letters, digits, '.', '_' and '-' only"
}

# Worktrees registered with the main checkout that live under $WT, one shell path per line.
registered() {
    local line
    g "$MAIN" worktree list --porcelain | while IFS= read -r line; do
        case "$line" in
            "worktree "*)
                line="$(sp "${line#worktree }")"
                case "$line" in "$WT"/*) printf '%s\n' "$line" ;; esac ;;
        esac
    done
}
is_registered() {   # (captured first: grep -q quitting early would SIGPIPE the loop, and pipefail)
    local all
    all="$(registered)"
    grep -qxF -- "$1" <<< "$all"
}

on_origin() {   # on_origin <tree>: prints an origin branch containing its HEAD, or nothing
    local head
    head="$(g "$1" rev-parse HEAD)" || return 0
    g "$MAIN" branch -r --contains "$head" 2> /dev/null | sed 's/^ *//' | grep -v -- ' -> ' | head -n1 || true
}

# Ignored entries that are not build output or caches.
unsafe_ignored() {
    g "$1" status --porcelain --ignored | sed -n 's/^!! //p' | grep -vE \
        '^build/|(^|/)__pycache__/|\.pyc$|(^|/)node_modules/|(^|/)\.wrangler/' || true
}

cmd_new() {
    [ $# -ge 1 ] && [ $# -le 2 ] || usage
    local name="$1" branch="${2:-$1}" path build from
    check_name "$name"
    "$GIT" check-ref-format --branch "$branch" > /dev/null 2>&1 || die "bad branch name '$branch'"
    path="$WT/$name"
    build="$HOME/strata-build-$name"
    [ ! -e "$path" ] || die "$path already exists"
    mkdir -p "$WT"

    echo "fetching origin"
    g "$MAIN" fetch -q origin
    if g "$MAIN" show-ref -q --verify "refs/heads/$branch"; then
        from="local branch $branch"
        g "$MAIN" worktree add -q "$(gp "$path")" "$branch"
    elif g "$MAIN" show-ref -q --verify "refs/remotes/origin/$branch"; then
        from="origin/$branch"
        g "$MAIN" worktree add -q --track -b "$branch" "$(gp "$path")" "origin/$branch"
    else
        from="origin/main"
        g "$MAIN" worktree add -q --no-track -b "$branch" "$(gp "$path")" origin/main
    fi
    echo "checking out submodules"
    g "$path" submodule update -q --init
    mkdir -p "$build"

    echo
    echo "worktree  $(gp "$path")"
    echo "branch    $branch (from $from)"
    echo "build     $build (tools/env.sh picks it in this worktree)"
    echo "from WSL  cd $path"
}

cmd_list() {
    [ $# -eq 0 ] || usage
    local path name branch state pushed size n=0
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        n=$((n + 1))
        name="$(basename "$path")"
        if [ ! -d "$path" ]; then
            printf '%-24s missing on disk (git worktree prune clears it)\n' "$name"
            continue
        fi
        branch="$(g "$path" symbolic-ref -q --short HEAD || echo "(detached)")"
        if [ -n "$(g "$path" status --porcelain)" ]; then state=dirty; else state=clean; fi
        if [ -n "$(on_origin "$path")" ]; then pushed=pushed; else pushed=unpushed; fi
        size="-"
        [ -d "$HOME/strata-build-$name" ] && size="$(du -sh "$HOME/strata-build-$name" 2> /dev/null | cut -f1)"
        printf '%-24s %-36s %-5s %-8s build %s\n' "$name" "$branch" "$state" "$pushed" "$size"
    done < <(registered)
    [ "$n" -gt 0 ] || echo "no worktrees under $WT"
}

cmd_rm() {
    local name="" keep_branch=0 drop_ignored=0 a
    for a in "$@"; do
        case "$a" in
            --keep-branch)  keep_branch=1 ;;
            --drop-ignored) drop_ignored=1 ;;
            -*)             usage ;;
            *)              [ -z "$name" ] || usage; name="$a" ;;
        esac
    done
    [ -n "$name" ] || usage
    check_name "$name"
    local path="$WT/$name" build="$HOME/strata-build-$name" branch dirty ignored origin_ref
    [ "$path" != "$MAIN" ] || die "refusing to remove the main checkout"
    [ -d "$path" ] || die "no worktree at $path"
    is_registered "$path" || die "$path is not a worktree of $MAIN"
    case "$(pwd -P)/" in "$(cd "$path" && pwd -P)"/*) die "this shell is inside $path: cd out first" ;; esac

    g "$MAIN" fetch -q origin || die "cannot fetch origin, so cannot tell what is pushed"
    dirty="$(g "$path" status --porcelain --ignore-submodules=none)"
    [ -z "$dirty" ] || { printf '%s\n' "$dirty" | sed 's/^/    /' >&2; die "$name has uncommitted changes (above): kept"; }
    origin_ref="$(on_origin "$path")"
    [ -n "$origin_ref" ] || die "$name: HEAD $(g "$path" rev-parse --short HEAD) is on no origin branch (unpushed commits): kept"
    ignored="$(unsafe_ignored "$path")"
    if [ -n "$ignored" ] && [ "$drop_ignored" -eq 0 ]; then
        printf '%s\n' "$ignored" | sed 's/^/    /' >&2
        die "$name holds ignored files that are not build output (above): kept. Look, then --drop-ignored"
    fi
    branch="$(g "$path" symbolic-ref -q --short HEAD || true)"

    echo "removing $(gp "$path") (HEAD on $origin_ref)"
    # --force: git refuses any worktree with an initialised submodule (third_party/enaga) without
    # it. The checks above are what protect work.
    g "$MAIN" worktree remove --force "$(gp "$path")"
    g "$MAIN" worktree prune
    if [ -n "$branch" ] && [ "$keep_branch" -eq 0 ]; then
        # Its tip is on origin (checked above), so -D loses nothing even when origin's copy sits
        # on a branch of another name.
        g "$MAIN" branch -q -D "$branch" && echo "deleted local branch $branch"
    fi
    if [ -d "$build" ]; then
        rm -rf -- "$build" && echo "deleted $build"
    fi
}

[ $# -ge 1 ] || usage
sub="$1"; shift
case "$sub" in
    new)        cmd_new "$@" ;;
    list|ls)    cmd_list "$@" ;;
    rm|remove)  cmd_rm "$@" ;;
    -h|--help)  usage ;;
    *)          usage ;;
esac
