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

- [ ] **Gate length is audible on VA POLY only.** `Step::length` is stored, edited on the step
      page, saved and restored, and `Engine::fire` now converts it to frames and sends it as
      `Trig::gate_frames`; VA POLY counts it down per voice and releases on its exact frame
      (9 host tests, 11 of 14 mutations caught, the three survivors recorded in
      `tests/test_vapoly.cpp`). Every OTHER machine is a one-shot built on a sustain-0 envelope
      and ignores the field, so on a drum track there is still nothing to hear: check it survives
      a save and reload there rather than listening for it. On a VA POLY track, listen: a short
      length should shorten the note, and a long one should not clip the decay.
- [ ] The conversion itself (`Engine::gate_frames_for`) is **not host-tested** — nothing in
      `app/` is, because the host suite builds only `core/` and `dsp/`. It reads
      `ticks_per_step`, `samples_per_tick_` and `length_sixteenths`, all of which ARE tested, but
      the multiplication is not. Listen for a plausible length rather than trusting it: at 120
      BPM a 1/16 step on speed x1 should gate for about 125 ms.
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

The Euclidean generator and KEYS mode so far. `core/` is host-tested and mutation-tested; the gesture is
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

### The three M6 interface decisions, and what each one settled

Three M6 features and one M5 leftover all wanted something `dsp/track.hpp`'s `Machine` did not
have. All three were decided before any of them was wired in, so the audio behaviour changes
once and `app/engine.cpp` was edited once. What was chosen, and what is still open:

Two of them are now also WIRED: VA POLY is on the CHORD track and `Engine::fire` expands a
step's chord byte onto its voices.

| The question | Decided | Still open |
|---|---|---|
| A per-note note-off, so `Step::length` can be heard | **A duration on the `Trig`.** `Trig::gate_frames` is additive — no new virtual, no note-to-voice map in the engine — and the machine releases its own voice when the count runs out. `Step::length` has been stored since M3 and makes a sound for the first time | A held KEYS key and `Cmd::release_note` still have no known duration, so they still cannot be released. A `release(note)` virtual can land later without disturbing the gate; it is not needed for a sequenced part |
| `render_add_stereo`, promised by a comment and declared nowhere | **The comment was wrong, and was corrected.** Machines are mono and the chain spreads them, which is what the rest of the code implements; VA POLY took pitch detune instead of stereo unison for the same reason | Whether a stereo machine is worth it at all. The cost is not the virtual but `InsertChain` being mono, so a stereo track doubles that track's insert work — on the two most expensive machines. That is a question for after a unit measurement, not before |
| Whether `Engine::fire` folds a MONO note into the scale | **Left as built, and documented in `app/engine.cpp` at the line.** `chord_expand` folds the step's note even for a type-`none` chord, so the asymmetry is POLY tracks vs MONO tracks, not chords vs notes: identical step data sounds different depending on which machine a track hosts | Folding everywhere is what docs/SPEC.md argues for, but it needs a notion of which machines are melodic that does not exist — and folding naively would retune the drums, since KICK reads `note` as a pitch. Machine switching is the change that gives the engine that notion; this belongs there |

- [ ] **Listen for the fold asymmetry.** Put the same note value on a MONO track and a POLY one
      in a scale that does not contain it. They should disagree, the POLY one being in key. If
      they sound wrong, that asymmetry is the first thing to suspect.

**VA POLY (`dsp/vapoly`)** — 41 host tests, 16 of 18 mutations caught on the machine and 11 of
14 on the gate (all five survivors honest, and named in `tests/test_vapoly.cpp`), and VaMono's
voice factored out and proven BIT-EXACT over 4,840 rendered values — still bit-exact after the
gate, which is why the countdown lives in VaPoly's `Slot` and not in the shared `VaVoice`.
**Registered on the CHORD track**, with chord expansion in `Engine::fire` and a MIN7 in the
default kit, so it sounds the moment the app starts: measured contributing a 0.09 peak to the
mix (-20.8 dBFS under the limiter) on every stab step, and 4.25 points of load per voice.

- [ ] Six notes sound together; a seventh steals the quietest started voice rather than being
      dropped.
- [ ] **A chord's six notes on the same frame all sound.** A voice is busy from the moment it
      takes a trig, not from the frame that trig starts on — the subagent found the bug this
      prevents via a surviving mutation, and the test is
      `vapoly_a_chord_change_that_steals_keeps_all_of_its_notes`.
- [ ] A ratchet's repeated note takes a second voice, so both hits sound.
- [ ] Slot 10 is **DET** (voice detune, up to a quarter semitone) where VA MONO has GLD, since
      glide and slide are MONO-only. "Spread" is therefore pitch, not stereo, and stereo unison
      is not implemented: machines are mono by decision, not by oversight (see the table above).
- [ ] **A gated chord.** Give a POLY step a short length: all six notes should end together and
      FADE rather than cut. Then a chord whose notes carry different lengths via locks — the
      countdown is per voice, so they should end apart.
- [ ] A gate shorter than the attack. The note should still speak before it releases, not click.
- [ ] **The six-voice cost, with cl6x. Without it, it is measured and it is bad:** 4.25 points
      per voice, so 25.6% of the block for six, against SPEC's 11,208 cycles for the whole
      machine -- 11.3x. The derived estimate in this box used to read ~2.0% for all six; it was
      wrong by an order of magnitude. docs/BUDGET.md has the four-point measurement, the three
      possible explanations and what to reach for. **This is the measurement the project most
      needs from the unit**, because every option for fixing it is work that should not start
      until someone knows whether cl6x already has.

**Chords (`core/chord`)** — 27 host tests, 15 of 15 mutations caught, and `Engine::fire` now
calls it: a POLY step's chord byte is expanded onto VA POLY's voices on one frame. The expansion
itself has no host test, because nothing in `app/` does — so the boxes below are the only check
on it.

- [ ] **The default kit's stab is a MIN7.** It is in `build_default_beat` so the chord path runs
      every time anyone plays the default kit, the way the two parameter locks are. Four notes
      should sound, together, on steps 1 and 9 of track 7. One note means the expansion is not
      running; a flam means the notes are not landing on the same frame.
- [ ] A POLY step with each chord type sounds like its name. The table is semitone stacks folded
      into the key, which gives the diatonic triad on every degree of a mode — so MAJ on the root
      of a minor key correctly comes out MINOR. Verify that reads as a feature and not a bug.
- [ ] Walking the chord field in the step page reaches every chord: the packing is 6 bits because
      `core/stepedit.cpp` already publishes the field as 0..63, and 58 is the highest byte it
      produces.
- [ ] A cleared pattern plays single notes. Type 0 with inversion 0 is 0, so zeroed memory is one
      note rather than a chord nobody asked for.
- [ ] Inversions move the voicing without changing the notes. Depth is clamped to one less than
      the note count, because deeper would transpose rather than invert.
- [ ] **The fold asymmetry is deliberate, not a bug to report.** `core/chord` folds the chord's
      root into the scale and `Engine::fire` passes a MONO step's note through unfolded, so the
      same note value plays differently on a POLY track than on a MONO one. The decision and its
      reasons are in the table above and in `app/engine.cpp` at the line; it is revisited when
      machine switching lands.

**KEYS mode** — the one M6 feature that IS scenario-tested (`tests/scenario_m6.sh` plays three
keys with REC on and watches them land in the pattern), so these are about feel rather than
whether it works.

- [ ] F1 + F4 opens KEYS; the screen shows all sixteen notes under the keys, roots brighter.
- [ ] The sixteen keys are sixteen DEGREES of the project's scale, not sixteen semitones: in a
      seven-note scale they should span just over two octaves and every one should be in key.
- [ ] The octave keys move the octave; the selection keys change track without leaving the mode.
- [ ] Change the key or scale in F3 and the KEYS layout follows it.
- [ ] **A key has no release.** `dsp/track.hpp`'s Machine interface has `release_all()` and
      nothing per-note, so releasing one key of a held chord would silence the whole machine. A
      long VA note therefore plays its own decay rather than stopping when you let go. If that
      feels wrong on the unit, the fix is a per-note release on the machine interface, and
      `Cmd::release_note` is sitting in the enum unimplemented waiting for it.

**GRAIN (`dsp/grain`)** — 46 host tests, 64,759 checks, and **not one of these boxes can be
ticked, because GRAIN cannot be reached.** docs/SPEC.md's default kit fills all eight tracks with
other machines, so there is no slot for it until a track's machine is switchable. That is M6/M7
work and it is not written. The code is verified to the extent host tests can verify anything:

- Compiles and LINKS for the C674x. Verified by forcing every entry point into the link
  (`machines_` leaves it unreferenced and `--gc-sections` drops the code, so a plain build proves
  only that it compiles): with the references in, APP.ENA goes from 422,600 to 439,568 bytes and
  `build_app.py`'s own "no undefined symbols" check passes — so no 64-bit divide, which on this
  toolchain drags in the ARM unwinder and stops the whole app linking (seawaffle/enaga#1).
- **No out-of-range read**, under AddressSanitizer rather than by argument: the whole suite built
  with ASan at -O1 reports zero buffer overflows over all 812 tests. That is the one check the
  machine's own oracle test cannot make, because a read that lands out of range with an
  interpolation weight of zero contributes nothing to hear.
- Allocates nothing — not even in `init()`. It reads PCM the track already holds.
- Mutation-tested twice, independently: 24 of 26 by the agent that wrote it, then 13 of 14 by a
  set chosen from reading `grainloops.ti.c` fresh. The second pass found three untested
  behaviours the first had not — the window's interpolation, the upper end of its index clamp and
  the stereo read's second tap — and two oracles were added that kill all three. One survivor
  remains in each set and both are the same honest one: removing the window phase clamp changes
  no sample today, because the table's padding and not the clamp is what bounds the read.

- [ ] **Everything about how it sounds.** POS, SIZ, DEN, SPR, PIT, SHP: every one of them is
      unheard. The parameters are documented in `dsp/grain.hpp` and nothing has listened to one.
- [ ] **Its cost on a REAL sample.** Now measured in the emulator on the placeholder:
      **+1.6 points at the default density, +9.2 with all eight grains**, against SPEC's 4.3% --
      so 2.1x over at the extreme, GCC-only. That is a floor and not a worst case: the
      placeholder is 38 KiB and sits in cache, so eight grains reading it are eight streams over
      one resident buffer. Load a multi-second sample, turn SPR up, and watch what happens.
      `docs/BUDGET.md` has the four-point table.
- [ ] The cache behaviour of eight concurrent read streams, which is the part of the estimate
      that could not even be bounded. Under heavy SPR every grain starts at a fresh random
      offset, so a sprayed cloud may be almost all L1D misses. If the measurement disappoints,
      this is the first thing to look at, and the lever is grain count rather than the
      interpolator.
- [ ] **SPEC's "tempo-locked stretch" is served by POS under a step lock or a tempo-synced LFO**,
      not by the machine knowing the tempo — no `dsp/` file includes `<enaga/clock.hpp>` and
      `Machine::prepare` carries only `TrackParams`. That is an interpretation of a SPEC line
      rather than something SPEC spells out, and it is worth confirming against SPEC before the
      machine is reachable.
- [ ] No reverse grains. Not in SPEC's GRAIN row, and they would double every case in the bounds
      argument.

### Ten bugs a review of `app/` found, all fixed, none of them testable here

`app/` is 5,142 lines and the host suite compiles only `core/` and `dsp/`, so a review is the
only verification it gets. These are the fixes, and each one needs a gesture on the unit to
confirm — that is what the boxes are.

**Three shared one root cause**: M5 added a per-block copy of `performed_` into `effective_` so a
morph could be heard between steps, and it quietly reverted anything else that wrote `effective_`.
**All three now have host tests** (`tests/test_app.cpp`), so the boxes below are a confirmation
rather than the only check there will ever be.

- [ ] **Turn a knob in EDIT mode. It should be heard at once.** `Cmd::set_param` mirrored into
      `effective_` but did not set `perform_dirty_`, so `apply_performance` returned early,
      `performed_` kept the old value and the block copy put it back. Knob edits were inaudible
      until some unrelated performance control fired.
- [ ] **Load a project. The kit should SOUND like the one on screen.** Same mechanism via
      `params_from_kit()`: the previous project's kit went on playing under the new project's
      patterns.
- [ ] **Motion-record a knob. It should be heard on the pass it is recorded**, not the next one.
      `lock_delta` mirrored into `effective_` without setting the `locked_` bit, so the copy —
      which rewrites everything the mask does not claim — undid it before a sample was rendered.
      The comment in the code promised exactly what the code then prevented.

**And seven more:**

- [ ] **The sidechain duck works at all.** Both arms of its depth ternary were `0.0f`, so an M2
      SPEC feature was dead while the curve was still being rendered on every kick. Now 0.35 —
      **a number to settle by ear**, since SPEC gives attack, hold and release and no depth.
- [ ] The duck depth is not reachable from the panel: it is mixer state set in `Engine::init`
      rather than a kit parameter. Making it editable means a kit field.
- [ ] **Generate a Euclidean rhythm, save, reopen.** It should still be there. `Cmd::euclid` was
      missing from `mutates_pattern`, so the bank stayed clean and an explicit save skipped the
      bank file entirely.
- [ ] **Change the key or scale, save, reload.** It should hold. Both commands write
      `project_.settings` AND `live().root`/`.scale`, and only the project was marked dirty — so
      the bank's untouched copy won, which is the copy `core/scale` reads.
- [ ] **A WAV with a trailing chunk** (LIST, cue, id3 — most editors write one) should play its
      audio and not its metadata. The frame count came from "everything after the header" rather
      than from the data chunk's own length.
- [ ] **Hold a STEP key, change page or mode, let go, then turn an encoder.** No parameter lock
      should appear. The hold was resolved against the CURRENT page on release and against the
      press's page on the way in, and KEYS, PERFORM and MUTE swallow the STEP keys entirely — so
      a hold could be stranded forever, and the next encoder move wrote a lock onto a phantom
      step.
- [ ] **Press PLAY while a project is loading.** It should refuse and say `LOADING`, not start
      the transport over pattern memory that `step_load` is halfway through rewriting.
- [ ] **The unsaved-changes `*`.** Six gestures — mutate, a live-recorded hit, a queued pattern,
      a macro assignment, a morph move and a mixer level — built their command by hand and so
      skipped `note_command` completely, leaving the screen claiming the project was saved. All
      posts now go through one function; check the `*` appears for **five** of them.
- [ ] **The morph is the exception, deliberately.** `Cmd::set_morph` is in none of
      `app/storage.cpp`'s `mutates_*` lists, because the morph position is "where the performer's
      hands are, not part of the project". So moving VALUE in PERFORM must NOT raise the `*`,
      and `tests/test_ui.cpp` asserts the pair: the morph moved **and** the project stayed clean.
      If that feels wrong on the unit — if someone sets a morph, saves, reloads and is surprised
      it did not keep — the fix is to add it to a list, not to the UI.

### `app/` is host-tested now, and the first thing that found

`tests/fakehost.cpp` is a fake Enaga: `enaga::host()` returns a struct of 38 function pointers
that the app runtime normally defines, and `host/Makefile` does not link that runtime — so the
tests can define it instead and `app/engine.cpp` and `app/storage.cpp` compile for the host
exactly as they compile for the device, with no `#ifdef` in either. The clock is programmable and
a `files::load` does not complete until a test says so.

It found a data-loss bug before it had a single assertion in it, and a second one an hour later.
Both are fixed, both have regression tests, and both are listed here because the fix should be
confirmed by hand once:

- [ ] **Save a brand-new project, then reopen it.** `request_save` marks only the KIT dirty and
      `step_save` skips every file whose flag is clear — the flags describe how RAM differs from
      a project ON THE CARD, and for a project never written there is nothing to differ from. So
      a first save stored `KIT.BIN` alone and reopening it failed with NOT FOUND: the user's own
      work, reported as saved, and gone. An explicit save now writes all ten files when the
      project is not known to be complete on the card.
- [ ] **Swap SD cards, then save.** The "project is complete on the card" flag is now cleared on
      any card transition, because after a swap it is a claim about a card nobody has looked at.
      Without that, a save onto a fresh card would write only the dirty files again.
- [ ] **Press SAVE, pull the card, put it back.** The save is CANCELLED — `on_card(false)` clears
      the pending request along with any job — so the project stays unsaved with `NO CARD` as the
      only sign. Nothing is lost and asking again works. Asking the other way round (card out
      first, then SAVE) does complete by itself when a card arrives. Both are pinned by tests;
      **whether the cancelled one should resume on its own is a judgement for the unit.**

### And then `app/app.cpp` became testable too

`ENAGA_APP(Strata)` defines the package's entry points and belongs to a device build, so the
whole `Strata` class moved to `app/app.hpp` and `app/app.cpp` is now that one line. The move was
pure -- the device image is byte-identical across it -- and `tests/test_ui.cpp` includes the
header and drives the same class the unit runs, with no `#ifdef` anywhere in `app/`.

19 tests, 26 of 27 mutations caught. They pin the held-STEP state machine, tap against hold
against range, the mode changes, KEYS as scale DEGREES, the GRID encoder routing, PLAY's refusal
during a load, and the unsaved-changes mark. What they cannot reach is said in comments rather
than faked: the LED language and all screen TEXT, because the fake host counts LEDs and ignores
text. The screen is checked by pixels instead.

**A third way to strand a held STEP key came out of it, and it is fixed.** MUTE mode and the
scene pads swallow the STEP keys as well, and neither is a mode -- the performance row works in
every mode on purpose -- so they needed the hold dropped without the rest of a mode change.
Before: hold STEP 1, tap the MUTE pad, let go, and the next encoder turn wrote a parameter lock
onto a step nobody was touching.

- [ ] **Hold a STEP key, tap MUTE or a scene pad, let go, turn an encoder.** No lock should
      appear. Then confirm the pad itself still works: dropping the hold must not have swallowed
      the gesture it was pressed for.
- [ ] A tempo clamp sign bug was fixed on the way past: turning VALUE down hard used to wrap an
      unsigned subtraction and clamp to 300 BPM instead of 20. Not reachable from the panel (an
      encoder event carries a click or two), so this is a glance rather than a test.

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
