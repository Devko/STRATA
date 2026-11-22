# Releasing STRATA

A release is a zip for the SD card, made from the card that passed the unit test, and a GitHub
release that carries it. Nothing here is published until the unit test passes and Roland says go.

## 1. Before packaging

- [ ] The unit walkthrough at the top of `docs/UNIT_CHECKLIST.md` passed on the card being
      released, with the build noted on its first line (cl6x or GCC only).
- [ ] If the `APP.ENA` was built with cl6x: read the licence text in
      `~/ti/ti-cgt-c6000_8.5.0.LTS` (a `LICENSE`, `*manifest*` or `license` directory) for a
      condition on distributing the compiler's **output**, and record what it says in
      `docs/BUDGET.md`, "The cl6x licence question, examined". If it attaches one, stop and ask.
      If in doubt, release a GCC-only build (`ENAGA_TI_CGT=none tools/build.sh`) instead.
- [ ] `SmplTrek.bin` on the card is **Enaga's own v0.2.0 release file**, not a local build: its
      SHA-256 matches the one in the SmplTrek zip from
      <https://github.com/seawaffle/enaga/releases/tag/v0.2.0>. (The script checks it is Enaga
      0.2.0 for the SmplTrek and nothing else; it cannot tell a local build from the release.)
- [ ] The `APP.ENA` is the plain STRATA build, not STRATA6: `tools/build.sh` with no `--name`.
      The script refuses any package not named STRATA, and leaves `ENAGA/APPS/STRATA6/` out.

## 2. Fill in the notes

- [ ] Replace every `TODO(release)` in `CHANGELOG.md` and `docs/release/v0.1.0.md` with the
      unit test's result and the MAX readings.
- [ ] In `CHANGELOG.md`, change `## [0.1.0] - unreleased` to the release date,
      `## [0.1.0] - YYYY-MM-DD`.
- [ ] Commit both, on a branch, and merge it.

## 3. Package

In WSL, from the repository:

```sh
python3 -B tests/test_package_release.py           # the script's own tests
python3 -B tools/package_release.py v0.1.0            # build/card -> build/release/
```

It refuses while a `TODO(release)` is left or the changelog is undated; `--draft` makes a trial
zip named `...-draft.zip` before then. `--no-firmware` makes an `...-app-only.zip` with only
`APP.ENA`, whose README.txt points to Enaga's release instead. The output names both files'
SHA-256 and the zip's; `SHA256SUMS.txt` in the zip lists the card files.

Optional, and worth it: unzip it onto a freshly formatted card, install, and play A01.

## 4. Publish (Roland's go)

- [ ] Tag the merged commit: `git tag -a v0.1.0 -m "STRATA 0.1.0"` and push the tag.
- [ ] Create the GitHub release from tag `v0.1.0`, title **STRATA 0.1.0**, marked **pre-release**,
      body = `docs/release/v0.1.0.md` below its `---` line, with the SHA-256 filled in.
- [ ] Attach `STRATA-v0.1.0-smpltrek.zip` and its `.sha256`.
- [ ] Publish. Then update README.md's "Installing on a SmplTrek" (it says there is no prebuilt
      release yet) and the status badge, in a follow-up commit.

## Rules that do not change

- Never put an official Sonicware update file in a zip, and never one with BOOT or MCU sections.
  The script refuses both.
- Enaga's version in the zip is the pinned one. Moving it follows `docs/HANDOFF.md`, "Moving the
  Enaga pin", and the README.txt, the notes and this file move with it.
