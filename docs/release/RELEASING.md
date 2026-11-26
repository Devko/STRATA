# Releasing STRATA

A release is a zip for the SD card, made from the card that passed the unit test, and a GitHub
pre-release that carries it. **The zip holds STRATA only, never Enaga's firmware:** users install
Enaga from Enaga's own release, which the zip's README.txt and the release notes link. We do not
redistribute someone else's firmware, users get it from its authors rather than trusting our copy,
and a problem with the firmware file stays with its source. Nothing is published until the unit test passes and Roland says go.
Publishing is one command, `tools/publish_release.sh`, and it refuses rather than guesses.

## Before: the card is what ships

- [ ] The card was built from `main` as it is now (`tools/build.sh`, no `--name`) and the
      walkthrough at the top of `docs/UNIT_CHECKLIST.md` passed on it.
- [ ] The unit ran Enaga's own release **of the pinned version** for the test (v0.3.0 since
      2026-11-25: `enaga-v0.3.0-smpltrek.zip`,
      <https://github.com/seawaffle/enaga/releases/tag/v0.3.0>), not a local firmware build.
      Leave the `SmplTrek.bin` you installed from on the card: the script then checks it is
      Enaga's file, and leaves it out of the zip either way.
- [ ] The release notes (`docs/release/vX.Y.Z.md`) name the Enaga version, link its release,
      and say the zip does not include it. If the Enaga version changed since the last release,
      they say so, since updating then needs step 1 of docs/INSTALL.md again. (v0.1.0 to v0.3.1
      bundled Enaga 0.2.0's `SmplTrek.bin`; their notes describe that and stay as published.)
- [ ] If the `APP.ENA` was built with cl6x: read the licence text in
      `~/ti/ti-cgt-c6000_8.5.0.LTS` (a `LICENSE`, `*manifest*` or `license` directory) for a
      condition on distributing the compiler's **output**, and record what it says in
      `docs/BUDGET.md`, "The cl6x licence question, examined". If it attaches one, stop and ask.
      If in doubt, release a GCC-only build (`ENAGA_TI_CGT=none tools/build.sh`) instead.
- [ ] `gh` is installed in WSL and logged in (`gh auth status`).

## Publish: one command

In WSL, from a clean checkout of `main`:

```sh
tools/publish_release.sh v0.1.0 --build gcc --max-a01 30.1 --max-a04 41.7 --dry-run   # read it first
tools/publish_release.sh v0.1.0 --build gcc --max-a01 30.1 --max-a04 41.7             # publish
```

Without the clean A01 and A04 readings, `--unit-load "One sentence."` puts your own sentence about the
unit's load in their place.

`--build` says which compiler built the card (`gcc`, or `cl6x` together with
`--ti-licence-read`), and the two figures are the MAX readings on A01 and A04 from the
walkthrough. `--draft` creates the GitHub release as a draft to look at before pressing Publish.
`--card DIR` takes a card folder other than `build/card`, and `--date` overrides today's date.

What it does, refusing at the first thing that is wrong:

1. The tree is clean, HEAD is `origin/main`, and neither the tag nor the release exists.
2. It rebuilds `APP.ENA` from HEAD with the same compiler and compares it byte for byte with the
   card's, so the release is the code it is tagged as. (`--skip-build-check` only if the card was
   deliberately built elsewhere.)
3. It downloads Enaga's SmplTrek zip of the pinned version with `gh`, so the link users follow
   works, and, when the card still has a `SmplTrek.bin`, checks it is the file in that zip, byte
   for byte. (`--skip-enaga-check` only when GitHub is unreachable; open the release page and
   compare SHA-256 by hand then.)
4. It fills the `{{FIELDS}}` in `CHANGELOG.md` and `docs/release/v0.1.0.md` (date, build, MAX
   readings, the zip's SHA-256), runs `tests/test_package_release.py`, and builds the zip with
   `tools/package_release.py`. `--dry-run` prints the release text here and stops, leaving the
   tree as it was.
5. It commits the filled notes on `release/v0.1.0`, tags that commit `v0.1.0`, pushes both, creates
   the GitHub pre-release "STRATA 0.1.0" with the zip and its `.sha256`, and opens a PR that
   brings the dated changelog into `main`.

## After

- [ ] Merge the release PR.
- [ ] Update the README's "Status" section (current release and date) and, if the release changes
      the install steps or the Enaga version, the README's "Try it" and `docs/INSTALL.md`.
- [ ] Add the release to the card builder: a new first entry in `web/public/compat.json` with
      its version, date, the zip's SHA-256 (the `.sha256` file on the release page) and the Enaga
      version it needs. A new Enaga version also needs its own entry under `"enaga"`, with the
      SHA-256 of `enaga-vX.Y.Z-smpltrek.zip` as downloaded from Enaga's release.
      `cd web && npm test` checks the list. Once that is on `main`, deploy the site from
      `web/` with `npx wrangler pages deploy` ([web/README.md](../../web/README.md)); it is
      not connected to the repository, so nothing redeploys by itself.
- [ ] Optional, and worth it: download the zip from the release page, unzip it onto a freshly
      formatted card, install, and play A01.

## The pieces, for doing a step by hand

- `tools/package_release.py vX.Y.Z [--card DIR] [--out DIR] [--draft]` builds the zip alone. It
  takes only `ENAGA/APPS/STRATA/APP.ENA` from the card and lists everything else, `SmplTrek.bin`
  included, as left out. It refuses an `APP.ENA` not named STRATA or built for another ABI, a
  changelog with unfilled fields unless `--draft`, and, reading the finished zip back, any zip
  with a `.bin` in it.
- `docs/release/README.txt` is the README inside the zip; `docs/release/vX.Y.Z.md` is the release
  body (everything below its `---` line).

## Rules that do not change

- Never put firmware in a zip: not Enaga's `SmplTrek.bin`, and never an official Sonicware update
  file. The packaging script takes only `APP.ENA` and refuses a zip with a `.bin` in it.
- The Enaga version the README.txt and the notes link is the pinned one. Moving it follows `docs/HANDOFF.md`, "Moving the
  Enaga pin", and the README.txt, the notes and this file move with it.
