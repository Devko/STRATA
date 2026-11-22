#!/usr/bin/env bash
# Prints `code=true` or `code=false` for $GITHUB_OUTPUT: false only when a pull request touches
# nothing but documentation, so the build-and-test jobs can skip their heavy steps.
#
#   tools/ci_changes.sh EVENT_NAME
#
# The jobs still run and report success when they skip, so a required check is never left
# waiting -- that is why this is a step inside each job and not `paths-ignore` on the workflow.
# Repo checks (tools/enaga_check.sh reads CLAUDE.md and docs/ENAGA_API.md) always run in full.
#
# On a pull_request the checkout is GitHub's merge commit: its first parent is the base branch,
# so `HEAD^1..HEAD` is exactly what the PR changes (needs fetch-depth: 2). Anything else -- a
# push to main, a manual run, a diff that cannot be computed -- runs everything.
set -uo pipefail

event="${1:-}"
if [ "$event" != "pull_request" ]; then
    echo "code=true"
    exit 0
fi

if ! files=$(git diff --name-only HEAD^1 HEAD 2>/dev/null) || [ -z "$files" ]; then
    echo "code=true"
    exit 0
fi

# Documentation: Markdown anywhere, and docs/ (which holds only prose and images today). A file
# under tests/, tools/, core/, dsp/, app/, host/ or .github/ always counts as code, even a .md.
code=false
while IFS= read -r f; do
    case "$f" in
        tests/*|tools/*|core/*|dsp/*|app/*|host/*|.github/*|third_party/*) code=true ;;
        docs/*|*.md|LICENSE) ;;
        *) code=true ;;
    esac
done <<< "$files"

echo "code=$code"
if [ "$code" = false ]; then
    echo "documentation only, host tests skipped:" >&2
    printf '  %s\n' $files >&2
fi
