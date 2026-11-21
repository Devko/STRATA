# What to check at the next session on the unit

Only the unit settles CPU load, timing and feel. Everything here needs the hardware, so it
cannot be closed from a development machine. Work top to bottom and record figures in
`docs/BUDGET.md`.

## Before powering anything on — safety

From `docs/SPEC.md`, "Unit safety checklist". Do not skip these: a hung image leaves the power
key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin`. One is already at
      `D:\DEV\smpltrek\SmplTrek v3.2.174\SmplTrek.bin`.
- [ ] Confirm **HOME (F4) held at power-on** reaches BOOT MODE, before it is needed in anger.
- [ ] Run on the power adapter **without batteries**, so a hung image can be power-cycled.
- [ ] Confirm the build writes no SPI flash and ships no BOOT or MCU sections.

## M0 — skeleton

- [ ] `/ENAGA/APPS/STRATA/APP.ENA` on the card; the app appears in Enaga's app list and starts.
- [ ] The screen shows `STRATA`, the device name, LOAD, PEAK, CLK, BPM and PPQN.
- [ ] Device name reads **SMPLTREK** (it comes from the bootloader, so it proves
      `system::device()` works rather than a hardcoded string).
- [ ] Idle load and peak recorded in `docs/BUDGET.md`. Expect roughly Enaga's own ~3%: the
      skeleton outputs silence.
- [ ] No audible noise or clicks with the app running and the transport stopped.
- [ ] **Clock readout, the M1 foundation.** With CLOCK SRC = INTERNAL, confirm BPM tracks the
      unit's tempo setting and `RUN`/`STOP` follow the transport.
- [ ] With CLOCK SRC = MIDI and a clock arriving at MIDI IN, confirm the line shows `MIDI`,
      `EXT` and the incoming tempo. **This is the first time Enaga's MIDI DIN has run on real
      hardware** — see the note below.
- [ ] With CLOCK SRC = SYNC and pulses at SYNC IN, confirm the line shows `SYNC` and `EXT`.
- [ ] Leave it running for 10 minutes: no underruns, no watchdog reset, load stable.

## M1 — scheduler and drums

The default kit plays a four-track beat at 130 BPM as soon as PLAY is pressed; nothing needs
setting up first.

- [ ] PLAY (F5) starts the beat; the screen's top line shows `>` and the tempo.
- [ ] **The load criterion.** The emulator gives **12.02% peak with cl6x** for the four-track
      beat, against M1's bar of 15% — it passes there, after the `*.ti.c` extraction took it
      down from 20.14%. Measure it on the unit anyway: pure computation runs about 25% faster
      there, and this is the peak block rather than the average, so the real figure should be
      lower still. Record it in `docs/BUDGET.md`.
- [ ] **The same beat built GCC-only** (`ENAGA_TI_CGT=none`) for comparison: the emulator says
      21.64%. The gap is the thing to confirm on hardware, because the eight-track worst case
      is 23.61% with cl6x and 44.26% without — i.e. **GCC-only misses M2's 40% bar**, and that
      is what makes the cl6x redistribution question matter.
- [ ] STOP (F6) stops with tails; STOP twice cuts them.
- [ ] VALUE changes the tempo and the beat follows it smoothly.
- [ ] STEP 1-16 toggles trigs on the selected track and the change is heard on the next pass.
- [ ] The lower pads 1-8 select a track and play it with velocity; F1 + pad mutes.
- [ ] Encoders A-D edit the selected track's four parameters. **Check A and B are not swapped** —
      the encoder event ids are alphabetical while the push-button codes are not, and getting
      that wrong is silent (`docs/ENAGA_API.md`).
- [ ] Pad velocity feels right across all 15 pads; note any crosstalk.
- [ ] **Timing.** Record line out at 48 kHz and measure the spacing of the kick's onsets over
      5 minutes. M1's bar is jitter under 0.1 ms.
- [ ] The hats choke: the closed hat on track 3 cuts the open one on track 4.
- [ ] 10 minutes of playing: no underruns, no watchdog reset, load stable.

## M2 — the full kit

All eight track slots are filled: KICK, SNARE, HAT closed, HAT open, PERC, BASS, a mono stab
standing in for CHORD, and the LOOP sampler. The default pattern plays all of them.

- [ ] **The load criterion.** The emulator gives **17.00%** for the default kit and **23.61%**
      worst case (drive, LP24, lo-fi and both sends on all eight tracks) against M2's bar of 40%.
      Measure both on the unit and record them in `docs/BUDGET.md`.
- [ ] **The GCC-only comparison matters here.** The emulator says 44.26% worst case without
      cl6x, which misses the bar. Confirm the gap on hardware — it is the evidence behind the
      redistribution question in `docs/BUDGET.md`.
- [ ] PERC on track 5: the default is a rimshot. Check the ratio knob's lower half steps between
      *characters* (hollow at integer ratios, metallic at irrational ones) rather than just
      getting brighter.
- [ ] BASS on track 6: the filter should squelch with resonance up without self-destructing.
      **Accent should be audibly more than volume**, and the slide on step 11 should glide into
      its note rather than retrigger.
- [ ] LOOP on track 8 plays a **synthesised placeholder**, not a file — eight slices built in
      `Engine::init`. Check successive notes pick successive slices, and that the boundaries land
      on the blips (a slice-count mismatch puts them mid-blip).
- [ ] EDIT (F8) and MIXER (F7): the selection keys move the parameter page and the track bank.
      Check the MIXER's faders and the master peak behave, and that EDIT's four values match
      what the encoders do.
- [ ] Listen for aliasing on PERC and BASS at the top of their ranges, and for a click when a
      voice is stolen on any machine.
- [ ] **Parameter locks, and listen properly.** The default pattern ships with two: the third
      kick of the bar has a shorter body (KICK DEC locked on step 9) and one closed hat opens up
      (HAT DEC on step 11). Both must be *audible*, and the step after each must revert to the
      kit value.

      This needs an ear rather than a test. `app/` cannot be host-tested — it is the only part
      of the project that includes Enaga headers, so there is no unit test standing behind this
      path. It was also broken once: locks were applied around `trigger()` but the render loop
      then prepared from the unlocked kit every block, so a lock on a cutoff or a decay did
      nothing at all while a locked *note* worked fine. If locks ever look right on screen and
      sound wrong, that asymmetry is the first thing to suspect.
- [ ] While there: turn a knob on a track whose current step carries a lock on that same
      parameter. The knob should be heard at once, and the lock should win again from the next
      step.

## M3 — sequencer depth

Everything here is a gesture, and gestures are the part of this project with the least test
cover: `app/` is the only code that includes Enaga headers, so none of it is reachable from
`make -C host test`. The logic behind each one is host-tested in `core/` (undo, recording's
arithmetic, the step fields, copy and paste); what needs the unit is whether the gesture itself
is findable and whether it feels like an instrument.

`tests/scenario_m3.sh` covers one path end to end in the emulator — toggle a step, undo, redo,
each verified by its effect rather than by the absence of a crash — so if that scenario passes,
the command ring and the undo stack are known to work before you touch the hardware.

**Step editing**

- [ ] Tap STEP: the trig toggles. Hold STEP and turn A-D: that parameter is **locked on that
      step only**, and the trig does *not* toggle when the key comes up. The second half is the
      one to watch: if a held step toggles its trig on release, every lock edit also flips a trig.
- [ ] F1 + STEP makes a step **trigless**: its locks apply and no note sounds. The LED is red
      alone, where a trig is green. An empty step and a trigless step must be distinguishable.
- [ ] Hold STEP + OK opens the step page. All eleven fields reachable on three pages with the
      selection keys, and each one does what its name says. **Check the condition field
      specifically**: NONE, then the eight named conditions, then the 35 A:B pairs, clamping at
      both ends rather than wrapping.
- [ ] Hold two STEP keys: the pair is a selection (the screen says `SEL n-m`). CLR erases it; UP
      copies it, DOWN pastes it; A transposes it and B nudges its micro-timing.
- [ ] CLR with nothing held erases the **whole track**. Confirm F1 + CLR brings it straight back:
      that is the only thing that makes the bare gesture acceptable.

**Undo**

- [ ] F1 + CLR undoes, F1 + OK redoes, 16 levels deep. The CLR and OK keys light when there is
      somewhere to go, so the panel says whether undo will do anything.
- [ ] **One sweep is one level.** Hold a step and turn an encoder through forty clicks, then
      undo once: the whole sweep goes, not one click of it. If each click took a level, the
      fortieth would have thrown away everything else in the history.
- [ ] A live-recording pass is one level, and a motion pass is one level, however many steps
      they touched.
- [ ] Undo after changing a track's length or speed restores those too, and the sequencer keeps
      playing without a glitch at the moment it happens.

**Recording**

- [ ] F2 arms REC (the key lights red). With the transport running, pad hits land on the nearest
      step with velocity. F1 + VALUE sets the quantize amount, shown as `Q nnn`.
- [ ] **Quantize 100 vs 0.** At 100 every hit snaps dead to the step. At 0 it keeps where you
      played it, as micro-timing. Play deliberately late and listen for it.
- [ ] **Swing and recording must agree.** Set the pattern swing to 66 and play dead on the swung
      offbeat: it must record as micro 0, not as an offset. This is the one piece of recording
      arithmetic that is wrong in a way you can only hear — `core/record.cpp` tests it against
      `core/timing`, but the two agreeing on the unit is what counts.
- [ ] **Pad latency.** `kPadLatencyMs` in `core/record.hpp` is **6 ms, a placeholder** — the
      spec says "about 4-8 ms" and nobody has measured it. To measure: set quantize to 0, play
      along to the kick as precisely as you can for a bar, then read the recorded `micro` values
      on the step page. A consistent bias one way is the latency figure; put it in that constant
      and record it here. Everything else about recording is correct before this is done; hits
      just sit slightly late.
- [ ] **Quantize 0 on a slow track.** `Step::micro` is ±11 ticks, but a 1/4x step is 96 ticks and
      1/8x is 192, so on those tracks an off-grid hit mostly *cannot* be stored where it was
      played: it clamps, and the screen says `OFF GRID`. Confirm that message appears rather than
      the hit silently moving. (This is `Step::micro`'s range from `docs/SPEC.md`, not a bug in
      the quantizer. If it proves annoying in practice, the fix is a design decision about what
      grid a slow track records on.)
- [ ] **Motion recording.** With REC on and the transport running, turn an encoder: locks are
      written on the steps the playhead passes over, and **the knob is audible as you turn it**.
      If the sound only changes on the next pass of the loop, the effective-parameter mirror in
      `Engine::apply` has been lost and motion recording will read as doing nothing.
- [ ] MIDI notes at MIDI IN record exactly as pad hits do, channels 1-8 addressing tracks 1-8.
      This shares one stamping mechanism with the pads deliberately; if pads record correctly and
      MIDI does not, suspect Enaga's MIDI DIN rather than STRATA (see the note at the end).

**Polymeter**

- [ ] F1 + octave keys set the selected track's length, F1 + selection keys its speed; both show
      on the grid line as `L nn` and `X n`. Set track 2 to 15 steps against track 1's 16 and
      listen for the phase walking — that is the feature working.
- [ ] A track at 2x with micro-timing at the extremes still fires every step exactly once per
      pass (the case `core/timing.cpp`'s scan window was written for).

**Two things that are deliberately not finished**

- [ ] **Gate length does nothing audible yet, and that is expected.** `Step::length` is stored,
      edited on the step page, saved and restored, but every machine STRATA currently has is a
      one-shot: each one builds its amplitude envelope with a sustain of 0, so a note ends when
      its decay does and there is nothing for a gate to release. The field becomes audible with
      the first sustaining machine (VA POLY / KEYS, M6). Check it survives a save and reload
      rather than listening for it.
- [ ] **A full lock pool.** 1,024 locks per pattern. `lock_delta` compacts and retries before
      refusing, so this should take real work to reach; when it is reached the screen says
      `FULL n`. Try to reach it (eight locks on every step of several tracks) and confirm the
      message appears and nothing else misbehaves.

## M4 — projects and storage

Written from the cloud, so **none of it has run**: no device build, no emulator, no listening.
Every box here is open, and the first three are the ones that decide whether the rest is worth
checking.

The exit criterion from `docs/PLAN.md` is the hard one: *"a power cycle restores the last state;
pulling the card during a save, during sample loading and during resampling never corrupts the
project and never needs a restart."*

**Does it even run**

- [ ] `tools/build.sh` links. Everything compiles and every symbol resolves on a host, but no
      cross linker has seen it.
- [ ] `tests/scenario_m3.sh` still passes: `app/engine.cpp` changed underneath it.
- [ ] The app starts with no card in the slot. A first run with nothing to open must keep the
      built-in default pattern, not fail `init()`.
- [ ] Memory: `Storage::init` takes 64 MiB for the region plus **18 MiB of staging** (`raw_`
      11.5 MiB for one whole file, `src_` 5.8 MiB for a resample's source, both sized from
      `docs/SPEC.md`'s own 30 s in-RAM limit). That is a fixed cost, not per sample. Confirm it
      against the ~124 MiB app heap and record it in `docs/BUDGET.md`.

**Projects**

- [ ] F3 opens PROJECT. The bank grid shows A, the live pattern is boxed, and the top line's
      pattern now follows the chain rather than reading `A01` for ever.
- [ ] OK saves. `/ENAGA/DATA/STRATA/PROJECTS/<NAME>/` holds `PROJECT.BIN` (251 bytes),
      `KIT.BIN` (1,076 bytes) and a `BANK_x.BIN` per touched bank.
- [ ] Only dirty banks are written. Edit one pattern in bank A, save, and confirm no
      `BANK_B.BIN` appears.
- [ ] Power cycle. The project reopens on the pattern it was left on, not A01.
- [ ] F1 + OK reloads from the card and discards unsaved edits.
- [ ] F1 + CLR starts a new project under a NEW name (`NEW1`, `NEW2`...), so saving it cannot
      overwrite the project it came from.
- [ ] A load with the transport RUNNING stops it first. Under CLOCK SRC = MIDI or SYNC, where
      `clock::stop()` returns false, the load is REFUSED and the screen says `EXT CLOCK` — it
      must not load anyway.
- [ ] Hand-damage a `BANK_x.BIN` byte: the other 15 patterns still load and the screen says
      `PATTERN LOST`.

**Autosave**

- [ ] Stopping the transport writes `/ENAGA/DATA/STRATA/AUTOSAVE/`. A stop arriving from MIDI
      autosaves too, since it follows the transport and not the key.
- [ ] Leaving the app (Enaga's menu) runs `stop()` and the autosave lands inside the 3 s budget.
- [ ] With an autosave newer than the project, the next start opens the autosave. "Newer" is a
      counter in `SETTINGS.TXT` (`PROJSEQ` / `AUTOSEQ`), because `files::Info` carries no
      timestamp — confirm the counters move.

**Samples, and the swap protocol**

- [ ] A project whose kit names a WAV loads it at open, and the track stays silent while it
      loads rather than glitching.
- [ ] The `SMP` line shows `O` loaded, `.` loading, `X` failed, `-` empty, and the pool's used
      and budget in MiB.
- [ ] A 44.1 kHz file is resampled and plays at the right pitch. An 8-bit, 24-bit and 32-bit
      float file each load (`core/pcm` is host-tested; this is the wiring).
- [ ] A file over 30 s at a rate other than 48 kHz says `TOO LONG` rather than loading a
      fraction of itself.
- [ ] **The swap, while the track is sounding.** Hold a sample playing and load another on the
      same track. The protocol is clear → wait for `Status::sample_epoch` → release → load →
      hand over, and the failure it prevents is audible garbage rather than a crash. If it ever
      says `NO SWAP`, the epoch never moved and that is a real bug, not a slow card.
- [ ] A track whose new kit names no file loses its old sample on load instead of keeping the
      previous project's sound.

**Pulling the card — the exit criterion**

- [ ] Pull during a save. `files::save` writes `NAME.TMP` and replaces, so the project must be
      either the old one or the new one, never half.
- [ ] Pull during sample loading, and during resampling. The screen says `NO CARD`, nothing is
      written, and no restart is needed.
- [ ] Put it back: the samples the kit names are offered again automatically.
- [ ] Pull it, then save: refused, with `NO CARD` on screen.

**Queue and chains**

- [ ] STEP 1-16 in PROJECT queues that pattern; encoder A picks END / 16 / 32 / NOW.
- [ ] With the transport STOPPED a queued change switches at once, because there is no end of a
      pass to wait for.
- [ ] A change armed a few ticks late still lands on the boundary it just missed
      (`kQueueGraceTicks`), and the new pattern's step 0 has already gone by — `core/queue.hpp`
      says so and it should be audible at the seam.
- [ ] UP toggles the chain; starting one jumps to its first entry. CLR cancels a queued change.
- [ ] A pattern change clears the undo history (it belonged to the pattern left behind) and
      restarts the `1ST` conditions.

## M5 — performance layer

Also written in the cloud, so also unrun. `core/scale`, `core/mutate` and `core/perform` are
host-tested and mutation-tested; `dsp/repeat` is host-tested for correctness but its LOAD is
unmeasured, and that is the one figure that decides whether beat repeat ships.

The exit criterion from `docs/PLAN.md`: *"a 30-minute jam using only the performance row, F7
PERFORM and pattern changes, with no edit screens and no underruns."*

**Beat repeat — the load question first**

- [ ] `dsp/repeat` idle: the per-block cost of copying the block into the ring, with and without
      cl6x, into `docs/BUDGET.md`. It is always on, so it is a fixed tax on every block.
- [ ] `dsp/repeat` held: the per-block cost of the interpolated read. Worst case is the shortest
      loop at half rate.
- [ ] Worst case with eight tracks AND a repeat held still under 60%. If it is not, this is the
      effect to cut first -- `docs/SPEC.md` ranks cutting effects above cutting voices.
- [ ] The 2.93 MiB ring, plus M4's 64 MiB region and 18 MiB of staging, against the real heap.
      85 MiB before anything else, and the staging was never in the spec's table.
- [ ] Hold the pad through a tempo change: the loop length is derived from ticks, so it should
      follow without a click at the seam.
- [ ] Hold it for four seconds and release: the mix resumes where the music actually is, not
      where the repeat left off. That is intended (`dsp/repeat.hpp` says why) -- confirm it does
      not sound like a dropout.
- [ ] Each of the seven lengths, and the triplets against a metronome: 1/4T should be three in
      the time of two 1/4.
- [ ] Pitch-down an octave, by ear against a known tone.

**Mutation**

- [ ] MUTATE on a track, then F1 + CLR: the pattern comes back exactly. "OK keeps it, CLR
      reverts" is the existing undo and nothing else, so this is really a test that a mutation
      is exactly one undo level.
- [ ] Press MUTATE twice: two different results, not the same one twice.
- [ ] The same pattern, the same press count, after a save and a reload: the same result. Seeded
      from the pattern is the claim.
- [ ] Density up and down on a 16-step track: the hits redistribute evenly rather than one being
      added where there was room. Audible as the rhythm changing shape, which is intended.
- [ ] Notes on a melodic track with a scale set: everything stays in the key.

**Key and scale**

- [ ] F3 PROJECT, encoder B walks the key and C the scale; the screen reads "KEY C MINOR".
- [ ] The key wraps at B to C; the scale stops at both ends.
- [ ] Save, power cycle, reopen: the key and scale come back. **This is also the regression test
      for the root bug** -- `app/engine.cpp` used to store 60 in a field the loader reduces to
      `% 12`, so the default pattern's notes would have come back at the bottom of the MIDI
      range.
- [ ] The note names read correctly as flats: DB, EB, GB, AB, BB. If the font turns out to have a
      '#', sharps would be the more conventional spelling for the major keys.

**Scenes, morph and macros**

- [ ] Capture a scene, change the sound, capture a second, then sweep VALUE: the sound crosses
      from one to the other.
- [ ] A scene that stores only one track: sweeping it must not drag the other seven.
- [ ] A parameter only one scene stores fades to the KIT value at the far end, not back to
      itself. This is the morph rule most likely to feel wrong in the room, so listen for it.
- [ ] A macro with two targets, one inverted: one knob opening a filter while it closes a send.
- [ ] Macro setup and scenes survive a save and a reload (PERFORM.BIN, 2,424 bytes, next to
      PROJECT.BIN).
- [ ] A project with no PERFORM.BIN -- anything M4 wrote -- opens with empty scenes rather than
      refusing.

**Mutes and fill**

- [ ] A lower pad mutes with F1 in any mode, and MUTE mode (upper pad 3) makes the lower pads
      mutes outright. **With F1 the mute waits for the bar**, which is the deviation to check:
      the spec says "instantly or at the next bar" without saying which gesture picks.
- [ ] A mute armed for the next bar lands on the bar, and pressing three pads before it means
      the third, not all three.
- [ ] **The eight stored states are on STEP 1-8 while MUTE mode is on**, not the upper row.
      docs/SPEC.md says "recalled from the upper row", but that row is seven FIXED pads and
      there are eight states, so it cannot be literal. Recall on a tap, F1 for the next bar,
      VALUE held to store. If the row is what was meant, this is the thing to change.
- [ ] They survive a save: the states live in PROJECT.BIN.
- [ ] Auto-fill at 4, 8 and 16 bars fires on the LAST bar of the period, and FILL conditions
      follow it. F1 + UP in PERFORM picks the period.
- [ ] **Stop and start the transport, then check auto-fill.** The bar counter resyncs on a
      backwards jump; without that it would have been wrong for ten bars after every stop, which
      is the bug this box exists for.
- [ ] Holding FILL with the transport STOPPED still reads as held.

**The panel, and what is reachable at all**

Every tool is reachable from the panel, which is worth confirming one by one: a feature with no
gesture is the mistake this milestone made twice before it was caught.

- [ ] Upper row, left to right: FILL (hold), REPEAT (hold, VALUE picks the length, F1 the
      pitch-down), MUTE mode (toggle), scene A (hold + STEP picks), scene B, THROW (hold, F1 for
      reverb), MUTATE.
- [ ] F1 + F7 opens PERFORM: encoders A-D are the macros, VALUE is the morph.
- [ ] In PERFORM: OK captures a scene (the selected track only while a scene pad is held),
      F1 + OK erases one, **STEP 1-16 assigns that parameter of the selected track to the
      selected macro**, the octave keys pick which macro, CLR clears it, UP and DOWN walk the
      mutation, the selection keys its amount.
- [ ] A macro assigned in one press is immediately usable: the range runs from where the
      parameter is now to whichever end is further away, so the knob always has somewhere to go.
      That is a guess at intent — check it feels right, since the ranges are stored and a richer
      editor can refine them later.
- [ ] A throw released puts the stored send back by itself. Nothing was written to the kit, so
      there is nothing to undo and nothing to save.
- [ ] A lock still wins over a morph: lock a cutoff on one step, sweep the morph, and that step
      should keep its locked value while the others move.

## M6 — full concept (started)

Only the Euclidean generator so far. `core/` is host-tested and mutation-tested; the gesture is
on encoders, which the emulator cannot drive.

- [ ] F1 + A, B, C in GRID walk hits, the cycle and the rotation, and the screen reads
      "EUC 5/16 R2". **Encoders cannot be driven by the emulator** (`emulate.py` has `--key`
      only, and the core exposes `key_input` and nothing else), so this binding has never been
      exercised at all.
- [ ] A sweep of any of the three is ONE undo level, not forty. They share a gesture so
      core/undo coalesces them; F1 + CLR once should put the old rhythm back.
- [ ] 3 hits in 8 on a 16-step track plays the figure twice. That is the point of hits and the
      cycle being two numbers.
- [ ] Generating over a part you have shaped by hand replaces the rhythm and keeps the sound:
      locks, conditions, ratchets, micro-timing and probability all survive. Host-tested, but
      worth hearing once.
- [ ] A rotation of a whole cycle is a no-op and reports no change, so it costs no undo level.

## M0.5 — budget spike

Folded into the M2 worst-case measurement above — see `docs/BUDGET.md` for why. These two lines
stand only if that measurement is ever disputed.

- [ ] (If needed) the four original configurations measured **on the unit**, not the emulator.
- [ ] (If needed) go/no-go recorded on 8 full insert chains plus both sends.

## Standing checks for every milestone

- [ ] Worst-case load under the milestone's target, recorded with and without cl6x.
- [ ] One-hour soak with pattern changes and the performance tools; peak load and underrun
      count recorded afterwards.
- [ ] Card tests: pull the card during a save, during sample loading and during resampling.
      The app must recover without a restart (`on_card(false)` closes the dead handles).
- [ ] Timing: record line out at 48 kHz and measure 16th-note click spacing over 5 minutes.
      M1's bar is onset jitter under 0.1 ms.

## Expect to find Enaga bugs, not just STRATA bugs

Enaga's MIDI DIN, SYNC, USB MIDI, input routing and pad-pressure curve are all implemented but
**have never run on a unit** — they are emulator-tested only. Two further known gaps: the
original's SYNC polarity settings are not built (Enaga uses rising edges in, active-high out),
and its USB IDs are TinyUSB test values that its own design log says must not ship.

So when sync misbehaves, suspect the platform before the app. Report upstream with a failing
case; never patch `third_party/enaga` in this repo.

## Pad feel

- [ ] Velocity curve across all 15 pads: a light press should be playable, a hard press should
      reach 127 without clipping the feel. Enaga maps 127 at about 3,300 raw, with light
      presses around 1,500 and hard around 3,400.
- [ ] Crosstalk between neighbouring pads — Enaga lists this as an open item, so characterise
      it before tuning a per-pad curve.
