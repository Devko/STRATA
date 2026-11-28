#!/usr/bin/env bash
# tools/wt.sh against a scratch origin, main checkout and worktree folder, with plain git (the PC
# runs it with git.exe from WSL, which CI cannot). Run by CI ("Repo checks") and tools/ci_local.sh.
#
# Covers: new from origin/main and from an origin branch, a refused duplicate and bad name, the
# ENAGA_BUILD that tools/env.sh picks inside a worktree, and rm refusing untracked files, staged
# changes, unpushed commits and stray ignored files, then removing a clean pushed worktree with its
# submodule, build folder and local branch while leaving the others alone.
set -uo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

fail=0
ok()   { echo "ok    $1"; }
bad()  { echo "FAIL: $1"; fail=1; }
check() { local what="$1"; shift; if "$@"; then ok "$what"; else bad "$what"; fi; }
# refuses <what> <expected words in the error> <command...>: it must fail, for that reason.
refuses() {
    local what="$1" why="$2"; shift 2
    if "$@" > "$T/out" 2>&1; then bad "$what (it did not refuse)"; cat "$T/out"
    elif ! grep -qF -- "$why" "$T/out"; then bad "$what (refused, but not with '$why')"; cat "$T/out"
    else ok "$what"; fi
}

export HOME="$T/home" GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
# The submodule is a local folder, which git refuses to clone unless allowed.
export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=protocol.file.allow GIT_CONFIG_VALUE_0=always \
       GIT_CONFIG_KEY_1=init.defaultBranch GIT_CONFIG_VALUE_1=main
mkdir -p "$HOME"

q() { "$@" > /dev/null 2>&1; }

# A submodule standing in for third_party/enaga, and an origin holding the real tools/.
q git init "$T/sub" && echo enaga > "$T/sub/README" && q git -C "$T/sub" add . && q git -C "$T/sub" commit -m sub
q git init "$T/seed"
mkdir -p "$T/seed/tools"
cp "$repo/tools/wt.sh" "$repo/tools/env.sh" "$T/seed/tools/"
cp "$repo/.gitignore" "$T/seed/"
q git -C "$T/seed" submodule add "$T/sub" third_party/enaga
q git -C "$T/seed" add . && q git -C "$T/seed" commit -m seed
q git clone --bare "$T/seed" "$T/origin.git"
q git clone "$T/origin.git" "$T/DEV/STRATA"
q git -C "$T/DEV/STRATA" submodule update --init
# A branch that exists only on origin.
q git -C "$T/seed" checkout -b claude/thread-b && echo b > "$T/seed/b.txt" &&
    q git -C "$T/seed" add b.txt && q git -C "$T/seed" commit -m b &&
    q git -C "$T/seed" push "$T/origin.git" claude/thread-b

MAIN="$T/DEV/STRATA" WT="$T/DEV/STRATA-wt"
wt() { STRATA_WT_GIT=git "$MAIN/tools/wt.sh" "$@"; }

check "new a starts branch a from origin/main" q wt new a
check "worktree a has the submodule checked out" test -f "$WT/a/third_party/enaga/README"
check "branch a is checked out in it" test "$(git -C "$WT/a" symbolic-ref --short HEAD)" = a
check "build folder for a exists" test -d "$HOME/strata-build-a"
check "env.sh in worktree a picks strata-build-a" \
    test "$(env -u ENAGA_BUILD bash -c ". '$WT/a/tools/env.sh'; echo \$ENAGA_BUILD")" = "$HOME/strata-build-a"
check "env.sh in the main checkout keeps strata-build" \
    test "$(env -u ENAGA_BUILD bash -c ". '$MAIN/tools/env.sh'; echo \$ENAGA_BUILD")" = "$HOME/strata-build"
refuses "new a again refuses" "already exists" wt new a
refuses "a name with a slash refuses" "bad name" wt new ../x
refuses "a name starting with '-' refuses" "bad name" wt new -x
check "new b tracks origin's claude/thread-b" q wt new b claude/thread-b
check "worktree b has origin's commit" test -f "$WT/b/b.txt"
check "b's upstream is origin/claude/thread-b" \
    test "$(git -C "$WT/b" rev-parse --abbrev-ref '@{u}')" = origin/claude/thread-b
check "list names both" bash -c "STRATA_WT_GIT=git '$MAIN/tools/wt.sh' list | grep -q '^a ' &&
    STRATA_WT_GIT=git '$MAIN/tools/wt.sh' list | grep -q '^b '"

echo x > "$WT/a/scratch.txt"
refuses "rm refuses an untracked file" "uncommitted changes" wt rm a
check "  ... and a is still there" test -f "$WT/a/scratch.txt"
git -C "$WT/a" add scratch.txt
refuses "rm refuses a staged change" "uncommitted changes" wt rm a
q git -C "$WT/a" commit -m local
refuses "rm refuses an unpushed commit" "unpushed commits" wt rm a
check "  ... and its build folder is still there" test -d "$HOME/strata-build-a"
q git -C "$WT/a" push -u origin a
mkdir -p "$WT/a/.claude/worktrees/agent" && echo notes > "$WT/a/.claude/worktrees/agent/n"
refuses "rm refuses ignored files that are not build output" "--drop-ignored" wt rm a
rm -rf "$WT/a/.claude"
mkdir -p "$WT/a/build" "$WT/a/tools/__pycache__" && echo o > "$WT/a/build/APP.ENA" && echo c > "$WT/a/tools/__pycache__/x.pyc"
refuses "rm refuses from inside the worktree" "cd out first" bash -c "cd '$WT/a' && STRATA_WT_GIT=git tools/wt.sh rm a"
check "rm removes a clean, pushed worktree (build output is fine)" q wt rm a
check "  ... the folder is gone" test ! -e "$WT/a"
check "  ... git no longer lists it" bash -c "! git -C '$MAIN' worktree list | grep -q '/STRATA-wt/a '"
check "  ... the build folder is gone" test ! -e "$HOME/strata-build-a"
check "  ... the local branch is gone" bash -c "! git -C '$MAIN' show-ref -q --verify refs/heads/a"
check "  ... origin's branch stays" git -C "$MAIN" show-ref -q --verify refs/remotes/origin/a
check "  ... b is untouched" test -f "$WT/b/b.txt"
check "  ... the main checkout is untouched" test "$(git -C "$MAIN" symbolic-ref --short HEAD)" = main

mkdir -p "$WT/b/.claude/worktrees/agent" && echo notes > "$WT/b/.claude/worktrees/agent/n"
check "rm --keep-branch --drop-ignored removes b" q wt rm b --keep-branch --drop-ignored
check "  ... and keeps its local branch" git -C "$MAIN" show-ref -q --verify refs/heads/claude/thread-b
refuses "rm of a missing worktree refuses" "no worktree at" wt rm nothere
mkdir -p "$WT/plain"
refuses "rm of a folder that is not a worktree refuses" "is not a worktree of" wt rm plain
check "  ... and leaves it" test -d "$WT/plain"

[ "$fail" -eq 0 ] && echo "ok    tools/wt.sh"
exit $fail
