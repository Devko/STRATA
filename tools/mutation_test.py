#!/usr/bin/env python3
"""Mutation testing for STRATA's host suite.

    python3 tools/mutation_test.py app/engine.cpp [more files...] [-j 4] [--lines 100-300]

Every mutant is one small edit to one line of one source file: a relational, equality or
logical operator flipped, an arithmetic operator swapped, a boolean literal inverted, a small
integer constant moved by one, or an expression statement deleted. Each is compiled with the
suite's own flags, linked against the objects of an ordinary `make -C host build`, and run
against the whole suite with a fail-fast runner. A mutant that compiles and passes every test
SURVIVES: either a test is missing, or the line changes no behaviour and the owning file should
say why (docs/PLAN.md, "Mutation testing is the house standard").

Needs `make -C host build` first, so build/host holds an object for every source. Nothing in the
tree is modified: each worker mutates a private copy under build/mutation/.

Output: one line per survivor, `file:line: description`, and a summary. --json writes every
mutant's outcome too.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "host")
WORK = os.path.join(ROOT, "build", "mutation")
CXXFLAGS = ["-std=c++20", "-fno-exceptions", "-fno-rtti", "-Wall", "-Wextra", "-Werror", "-O1"]
TIMEOUT = 60  # seconds; the suite takes about 5, so a mutant past this has made a loop endless


def mask(line):
    """The line with string/char literals and // comments blanked, so that no mutation lands
    inside one. Same length as the input, so offsets carry over."""
    out = list(line)
    i, n = 0, len(line)
    while i < n:
        c = line[i]
        if c in "\"'":
            q = c
            j = i + 1
            while j < n and line[j] != q:
                j += 2 if line[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = "_"
            i = j + 1
            continue
        if line.startswith("//", i):
            for k in range(i, n):
                out[k] = " "
            break
        i += 1
    return "".join(out)


# (pattern on the masked line, replacement, description). Patterns are applied one match at a
# time, so a line with two `<` yields two mutants.
OPS = [
    (r"(?<![<>=!])==(?!=)", "!=", "== -> !="),
    (r"!=", "==", "!= -> =="),
    (r"(?<![<>-])<=", "<", "<= -> <"),
    (r"(?<![<>-])>=", ">", ">= -> >"),
    (r"(?<![<>=\-\w:])<(?![<=])(?=\s)", "<=", "< -> <="),
    (r"(?<![<>=\-])>(?![>=])(?=\s)(?<=\s>)", ">=", "> -> >="),
    (r"&&", "||", "&& -> ||"),
    (r"\|\|", "&&", "|| -> &&"),
    (r"(?<=\s)\+(?=\s)", "-", "+ -> -"),
    (r"(?<=\s)-(?=\s)", "+", "- -> +"),
    (r"(?<=\s)\*(?=\s)", "/", "* -> /"),
    (r"\+=", "-=", "+= -> -="),
    (r"-=", "+=", "-= -> +="),
    (r"\+\+", "--", "++ -> --"),
    (r"\btrue\b", "false", "true -> false"),
    (r"\bfalse\b", "true", "false -> true"),
    (r"(?<![\w.])0(?![\w.xX])", "1", "0 -> 1"),
    (r"(?<![\w.])1(?![\w.xX])", "0", "1 -> 0"),
    (r"(?<![\w.])1(?![\w.xX])", "2", "1 -> 2"),
    (r"(?<=[(\s])!(?=[\w(])(?!=)", "", "! removed"),
]

DECL = re.compile(
    r"^\s*(const|constexpr|static|auto|bool|int|float|double|unsigned|uint\d+_t|int\d+_t|size_t|"
    r"char|void|return|case|default|struct|class|using|namespace|template|typedef|enum|break|"
    r"continue|goto|if|else|for|while|do|switch|public|private|protected|friend)\b")


def mutants_for(path, lo, hi):
    with open(path) as f:
        lines = f.read().split("\n")
    in_block = False
    for idx, line in enumerate(lines):
        no = idx + 1
        if not (lo <= no <= hi):
            # still track block comments
            pass
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            continue
        if not (lo <= no <= hi):
            continue
        if not s or s.startswith("//") or s.startswith("#") or "static_assert" in s:
            continue
        m = mask(line)
        for pat, rep, desc in OPS:
            for hit in re.finditer(pat, m):
                new = line[:hit.start()] + rep + line[hit.end():]
                yield no, desc, new
        # Statement deletion: a whole-line expression statement (call or assignment), not a
        # declaration, a return or a control statement.
        if (s.endswith(";") and not DECL.match(m) and "{" not in s and "}" not in s
                and not s.startswith("(") and (s.count("(") == s.count(")"))):
            prev = lines[idx - 1].strip() if idx else ""
            # a line continuing the previous one is not a statement of its own
            if prev.endswith((";", "{", "}", ":")) or prev.startswith("//") or not prev:
                indent = line[:len(line) - len(line.lstrip())]
                yield no, "statement deleted", indent + ";"


def setup_worker(i, rel_files):
    wdir = os.path.join(WORK, "w%d" % i)
    if os.path.isdir(wdir):
        shutil.rmtree(wdir)
    os.makedirs(wdir)
    for d in ("core", "dsp", "app", "tests"):
        shutil.copytree(os.path.join(ROOT, d), os.path.join(wdir, d))
    # fail-fast runner: the first failed check ends the run
    runner = os.path.join(wdir, "tests", "runner.cpp")
    src = open(runner).read()
    src = src.replace("    Registry::failures++;\n    std::printf(\"  FAIL",
                      "    Registry::failures++;\n    std::fflush(stdout);\n    std::_Exit(1);\n"
                      "    std::printf(\"  FAIL", 1)
    if "std::_Exit(1)" not in src:
        sys.exit("could not make the runner fail fast; tests/runner.cpp changed shape")
    src = src.replace("#include <cstring>", "#include <cstdlib>\n#include <cstring>", 1)
    open(runner, "w").write(src)
    # Link in the Makefile's order (sorted per directory: core, dsp, app, tests, then the C), so
    # the tests register and run in the order `make -C host test` runs them.
    skip = {os.path.join(OUT, f[:-4] + ".o") for f in rel_files}
    skip.add(os.path.join(OUT, "tests", "runner.o"))
    objs = []
    for d in ("core", "dsp", "app", "tests"):
        for f in sorted(os.listdir(os.path.join(ROOT, d))):
            p = os.path.join(OUT, d, f[:-4] + ".o")
            if f.endswith(".cpp") and p not in skip and os.path.exists(p):
                objs.append(p)
    # Then the C: every *.c under core/ and dsp/ (the *.ti.c hot loops) and Enaga's DSP runtime.
    for d in (os.path.join(OUT, "core"), os.path.join(OUT, "dsp"),
              os.path.join(OUT, "third_party", "enaga", "runtime", "dsp")):
        if not os.path.isdir(d):
            continue
        src_dir = os.path.join(ROOT, os.path.relpath(d, OUT))
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".c"):
                p = os.path.join(d, f[:-2] + ".o")
                if os.path.exists(p) and p not in objs:
                    objs.append(p)
    inc = ["-I" + os.path.join(wdir, d) for d in ("core", "dsp", "app", "tests")]
    inc.append("-I" + os.path.join(ROOT, "third_party", "enaga", "include"))
    run_o = os.path.join(wdir, "runner.o")
    subprocess.check_call(["g++"] + CXXFLAGS + inc + ["-c", runner, "-o", run_o])
    objs.append(run_o)
    # the unmutated objects of the files under test, built here with the same flags
    base = {}
    for f in rel_files:
        o = os.path.join(wdir, os.path.basename(f) + ".base.o")
        subprocess.check_call(["g++"] + CXXFLAGS + inc + ["-c", os.path.join(wdir, f), "-o", o])
        base[f] = o
    return {"dir": wdir, "inc": inc, "objs": objs, "base": base}


def run_mutant(w, rel, no, new_line, all_files):
    path = os.path.join(w["dir"], rel)
    orig = open(os.path.join(ROOT, rel)).read().split("\n")
    lines = list(orig)
    lines[no - 1] = new_line
    open(path, "w").write("\n".join(lines))
    try:
        mo = os.path.join(w["dir"], "mut.o")
        r = subprocess.run(["g++"] + CXXFLAGS + w["inc"] + ["-c", path, "-o", mo],
                           capture_output=True)
        if r.returncode != 0:
            return "invalid"
        objs = list(w["objs"]) + [mo] + [w["base"][f] for f in all_files if f != rel]
        exe = os.path.join(w["dir"], "t")
        r = subprocess.run(["g++", "-O1"] + objs + ["-o", exe], capture_output=True)
        if r.returncode != 0:
            return "invalid"
        try:
            r = subprocess.run([exe], capture_output=True, timeout=TIMEOUT, cwd=w["dir"])
        except subprocess.TimeoutExpired:
            return "killed"  # timeout
        return "survived" if r.returncode == 0 else "killed"
    finally:
        open(path, "w").write("\n".join(orig))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--lines", default="1-999999")
    ap.add_argument("--json")
    a = ap.parse_args()
    lo, hi = (int(x) for x in a.lines.split("-"))
    rel_files = [os.path.relpath(os.path.abspath(f), ROOT) for f in a.files]
    for f in rel_files:
        if not os.path.exists(os.path.join(OUT, f[:-4] + ".o")):
            sys.exit("build/host has no object for %s: run `make -C host build` first" % f)

    jobs = []
    for f in rel_files:
        for no, desc, new in mutants_for(os.path.join(ROOT, f), lo, hi):
            jobs.append((f, no, desc, new))
    print("%d mutants over %s, %d workers" % (len(jobs), ", ".join(rel_files), a.j), flush=True)

    workers = [setup_worker(i, rel_files) for i in range(a.j)]
    # The baseline: every file unmutated must pass in this harness, or every mutant would count
    # as killed. It has happened -- a test that depended on link order.
    first = rel_files[0]
    keep = open(os.path.join(ROOT, first)).read().split("\n")[0]
    if run_mutant(workers[0], first, 1, keep, rel_files) != "survived":
        sys.exit("the unmutated suite fails in the mutation harness; fix that first")
    print("baseline passes", flush=True)
    free = list(range(a.j))
    import threading
    lock = threading.Lock()
    results = []

    def task(job):
        with lock:
            wi = free.pop()
        try:
            res = run_mutant(workers[wi], job[0], job[1], job[3], rel_files)
        finally:
            with lock:
                free.append(wi)
        with lock:
            results.append((job, res))
            if res == "survived":
                print("%s:%d: %s   | %s" % (job[0], job[1], job[2], job[3].strip()), flush=True)
            if len(results) % 100 == 0:
                print("  ... %d/%d" % (len(results), len(jobs)), flush=True)
        return res

    with ThreadPoolExecutor(max_workers=a.j) as ex:
        list(ex.map(task, jobs))

    counts = {}
    for _, r in results:
        counts[r] = counts.get(r, 0) + 1
    valid = counts.get("killed", 0) + counts.get("survived", 0)
    print("\n%d killed, %d survived, %d did not compile; score %.1f%%" % (
        counts.get("killed", 0), counts.get("survived", 0), counts.get("invalid", 0),
        100.0 * counts.get("killed", 0) / valid if valid else 0.0))
    if a.json:
        with open(a.json, "w") as f:
            json.dump([{"file": j[0], "line": j[1], "op": j[2], "mutant": j[3], "result": r}
                       for j, r in sorted(results)], f, indent=1)


if __name__ == "__main__":
    main()
