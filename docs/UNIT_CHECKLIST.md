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
- [ ] **The load criterion.** The emulator gives 20.14% peak with cl6x against M1's bar of 15%.
      Measure it on the unit: pure computation runs about 25% faster there, and this is the peak
      block rather than the average, so the real figure should be lower. Record it in
      `docs/BUDGET.md` either way, and if it is still over 15%, the `*.ti.c` work is the lever
      (see that file's M1 note) rather than cutting a feature.
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

## M0.5 — budget spike

- [ ] All four configurations from `docs/BUDGET.md` measured **on the unit**, not the emulator.
- [ ] Go/no-go recorded on 8 full insert chains plus both sends.

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
