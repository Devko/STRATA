# Unit checklist: retired parts (2026-11-22)

Moved here from docs/UNIT_CHECKLIST.md on 2026-11-25. The controls tour was for the v0.1.0 card,
just after the controls remap (PR #74); the walkthrough at the top of UNIT_CHECKLIST.md now covers
what is still owed from it, and some of its gestures have moved since (CHANGELOG.md, "Gestures
that moved"). The release steps were for v0.1.0, which shipped on 2026-11-22; releases now follow
docs/release/RELEASING.md. Kept for the record, not to be run.

## The new controls (2026-11-22)

From the controls remap (PR #74), on the v0.1.0 card. Skip anything already checked there; today's
new items (KEYS on the pads, CLEAR PATTERN, macros, velocity, TUN) moved into the walkthrough above.

- [ ] **Performance pads lit.** On start the seven upper pads (printed TRK 1-7) glow dim: white,
      cyan, red, green, blue, amber, violet, left to right. Hold FILL: it brightens and the
      second line says `FILL`. Tap MUTE: red stays bright (`MUTE MODE`); again: dim.
- [ ] **Pads name themselves.** REPEAT says `REPEAT 1/16`, SCENE A `SCENE A: STEP PICKS`, THROW
      `THROW DELAY` (FUNC + THROW `THROW REVERB`), MUTATE `MUTATE T1 <type>` and stays bright
      violet until OK (`KEPT`) or CLR (`REVERTED`).
- [ ] **FUNC help.** Hold FUNC alone for a second: help for the screen that is up (since #107,
      walkthrough item 15); the screen keys turn to FUNC + REC ... EDIT and the upper pads. Let
      go: the screen is back. FUNC + a pad (a mute) never shows it.
- [ ] **FUNC lights while held**, green. **Mode keys:** HOME green on GRID, amber in KEYS, red in
      the menu; MIXER green / PERFORM red; EDIT green / MACHINE red; PROJECT green / SAMPLES red;
      REC green on RESAMPLE, red while recording.
- [ ] **HOME.** From any screen HOME is GRID. HOME twice quickly: `KEYS`. HOME again: GRID. A slow
      second press on GRID stays on GRID.
- [ ] **FUNC + PROJECT** ("pool") opens SAMPLES on the selected track. **FUNC + REC** RESAMPLE,
      **FUNC + MIXER** PERFORM, **FUNC + EDIT** MACHINE, as before.
- [ ] **The FUNC menu.** FUNC + HOME ("system"): `FUNC MENU`, cursor on SAVE, rows SAVE ... QUANTIZE
      with values on the right, nothing overlapping. UP/DOWN and A scroll; CLR goes back to the
      screen it opened over. **FUNC + HOME, OK saves** (`SAVING`).
- [ ] Menu rows: TRACK SPEED with LEFT/RIGHT changes the grid's `X n`; CHAIN shows ON/OFF and OK
      flips it; AUTO-FILL and QUANTIZE step with LEFT/RIGHT; NEW PROJECT needs OK twice
      (`OK AGAIN FOR NEW`); KITS opens the kit browser.
- [ ] **Screen keys = pages.** EDIT and MIXER as before. On PROJECT they walk PROJECT, CHAIN,
      PATTERN, SETUP, PROJECTS. On GRID they change the EDIT page the knobs turn (the second line
      says `FLTR` etc.); LEFT/RIGHT still flip GRID's step page.
- [ ] **Pad-row keys = which sixteen.** GRID, KEYS and PERFORM: step page. PROJECT: the bank
      (`BANK B`). KIT: nothing.
- [ ] **CHAIN:** CLR on an entry: `AGAIN TO REMOVE`; again: removed. On an empty chain CLR goes back.
- Notes on what still feels awkward: `____________________________________________`

## 18. If it passed: the v0.1.0 release

Only after the walkthrough passes and Roland says go. `docs/release/RELEASING.md` is the
procedure and wins where this summary differs.

- [ ] **The card is what ships.** It was built from `main` as it is now, the walkthrough passed
      on it, and `SmplTrek.bin` on it is Enaga's own v0.2.0 release file. If `APP.ENA` is a cl6x
      build, TI's licence text was read for conditions on its output and recorded in
      `docs/BUDGET.md`; if in doubt, release a GCC-only build.
- [ ] **One command** (WSL, on `main`, clean tree), with the build and the two MAX readings
      from section 3:
      `tools/publish_release.sh v0.1.0 --build gcc --max-a01 __._ --max-a04 __._`
      (`--build cl6x --ti-licence-read` for a cl6x card). Add `--dry-run` first to read the
      release text without publishing, or `--draft` to publish as a GitHub draft.
- [ ] Merge the `release/v0.1.0` PR it opens, then update README's install section and badge.
