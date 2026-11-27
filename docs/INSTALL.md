# Installing STRATA on a SmplTrek

> [!CAUTION]
> Installing third-party firmware is **at your own risk**. Neither Enaga nor STRATA is made,
> supported or endorsed by Sonicware, and installing them may affect your warranty. Keep an
> official update file at hand so you can restore the unit.

STRATA is an app for [Enaga](https://github.com/seawaffle/enaga), replacement firmware for the
SmplTrek. Installing it takes two downloads from two places:

| What | Where | What is in it |
| --- | --- | --- |
| **Enaga 0.3.0**, the firmware | [Enaga's v0.3.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.3.0): `enaga-v0.3.0-smpltrek.zip` | `SmplTrek.bin` |
| **STRATA**, the app | [STRATA's releases](https://github.com/Devko/STRATA/releases): `STRATA-v0.4.0-smpltrek.zip` | `SD-CARD/ENAGA/APPS/STRATA/APP.ENA`, a README.txt with these steps, the changelog and the licences |

The STRATA zip does not carry Enaga's firmware: you get that from its authors, not from a copy
of ours. (v0.1.0 to v0.3.1 did include Enaga 0.2.0's `SmplTrek.bin`.) Releases are
pre-releases until 1.0. To build `APP.ENA` yourself, see [BUILDING.md](BUILDING.md).

## The easy way: the card builder

[**strata.einblick.dev**](https://strata.einblick.dev) does both downloads for you. Pick a STRATA
release and it offers only the Enaga version that release needs; your browser then fetches the
two zips from their official GitHub releases, checks each against its published SHA-256, and
saves one zip laid out for the card:

```text
SmplTrek.bin                 Enaga's firmware, unchanged (left out with "STRATA only")
ENAGA/APPS/STRATA/APP.ENA    the STRATA app, unchanged
STRATA-README.txt            these steps, for that pair of versions
ENAGA-LICENSE.txt            Enaga's licence
STRATA-LICENSES.txt          STRATA's licence and every third-party notice
```

1. Do the [safety checks](#before-you-install) below first.
2. Copy everything in the zip to the **root** of the SD card. If the card already has an
   `ENAGA` folder, merge it and replace the old `APP.ENA`.
3. Hold **HOME (F4)** while switching on, choose **SYSTEM UPDATE**, and let it finish. This
   installs Enaga.
4. Switch on, and carry on at [step 3](#step-3-switch-on-and-play) below.

For an update on the same Enaga version (see the table below), choose **STRATA only**: the zip
then has no firmware, and step 3 is skipped. Nothing is uploaded and nothing is kept on the site;
its source is [web/](../web/README.md). The steps below do the same by hand.

## Before you install

From the safety section of [UNIT_CHECKLIST.md](UNIT_CHECKLIST.md). Do not skip these: a hung
image leaves the power key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin` from Sonicware, so you can restore the
      unit.
- [ ] Holding **HOME (F4)** while switching on reaches BOOT MODE.
- [ ] The unit runs on the power adapter **without batteries**, so a hung image can be
      power-cycled.

STRATA is for the SmplTrek only. The Lofi-12 XT and CyDrums are the same board, but STRATA has
not run on either.

## Which Enaga goes with which STRATA

Each STRATA release is built against one Enaga release, and the firmware on the unit must match
it **exactly**. Enaga's loader checks only an ABI number, which 0.2.0, 0.3.0 and 0.4.0 share, so a
mismatch starts and then misbehaves.

| STRATA | Enaga firmware |
| --- | --- |
| a build from `main` (since 2026-11-27) | [v0.4.0](https://github.com/seawaffle/enaga/releases/tag/v0.4.0) |
| v0.4.0 (the current release) | [v0.3.0](https://github.com/seawaffle/enaga/releases/tag/v0.3.0) |
| v0.1.0 to v0.3.1 | [v0.2.0](https://github.com/seawaffle/enaga/releases/tag/v0.2.0) |

## Step 1: install Enaga 0.3.0

Skip this step only if the unit already runs Enaga 0.3.0: the Enaga menu (**F1 + POWER**) shows
its version at the top.

1. Open [Enaga's v0.3.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.3.0) and
   download **`enaga-v0.3.0-smpltrek.zip`**. Not the `lofi12xt` or `cydrums` zip, and not the
   SDK or the toolchain.
2. Unzip it. It holds one file, `SmplTrek.bin`.
3. Copy `SmplTrek.bin` to the **root** of the SD card, keeping its name exactly.
4. Put the card in the SmplTrek. Hold **HOME (F4)** while switching on to reach BOOT MODE, and
   choose **SYSTEM UPDATE**. Let it finish before you touch anything.

Enaga's own [README, "Installing"](https://github.com/seawaffle/enaga#installing) has the same
steps.

A build from `main` needs **Enaga 0.4.0** instead: the same steps with `enaga-v0.4.0-smpltrek.zip`
from [Enaga's v0.4.0 release](https://github.com/seawaffle/enaga/releases/tag/v0.4.0).

## Step 2: copy STRATA to the card

1. Download `STRATA-v0.4.0-smpltrek.zip` from
   [STRATA's releases](https://github.com/Devko/STRATA/releases) and unzip it on your computer.
   (Optional: compare its SHA-256 with the `.sha256` file next to it on the release page.)
2. Copy `SD-CARD/ENAGA/APPS/STRATA/APP.ENA` from the zip to the card, at exactly this path:

   ```text
   /ENAGA/APPS/STRATA/APP.ENA
   ```

   Make the folders `ENAGA`, `APPS` and `STRATA` if the card does not have them yet, or copy the
   zip's whole `SD-CARD/ENAGA` folder onto the card's root. When updating, replace the old
   `APP.ENA`.

Once Enaga runs, the Enaga menu's **USB DRIVE** page puts the card on a computer over USB, so the
card can stay in the unit for this step.

## Step 3: switch on and play

1. Switch on. Enaga starts the last app that ran, or the only one installed.
   **F1 (FUNC) + POWER** opens the Enaga menu, which lists the apps and switches between them.
2. A first start opens the factory project: five patterns in bank A (A01 GROOVE, A02 GROOVE+,
   A03 BREAK, A04 PEAK, A05 PAD) at 130 BPM in C minor. Press **PLAY**.
3. In STRATA, hold **FUNC** to see what every key does where you are. **FUNC + HOME**, then
   **OK**, saves the project.

The [user manual](MANUAL.md) starts with a first-beat tutorial, and the
[cheatsheet](CHEATSHEET.md) has every control on one page.

## Updating

- **From STRATA 0.3.1 or earlier to 0.4.0:** the Enaga version changes (0.2.0 to 0.3.0), so do
  step 1, then step 2.
- **Between releases on the same Enaga version** (see the table above): step 2 only.
- **From 0.4.0 to a build from `main`:** the Enaga version changes (0.3.0 to 0.4.0), so step 1
  with Enaga 0.4.0, then step 2. The
  changelog's "Upgrading from 0.4.0" (under Unreleased) says what sounds different and what an
  older build drops from a project saved by the newer one.

Your projects stay on the card. Before 1.0 a release may change the project format; the
[changelog](../CHANGELOG.md) says when older projects play differently. Keep a copy of
`/ENAGA/DATA/STRATA/` on your computer before an update, so you can go back to the older build
with your projects as they were.

## Where STRATA keeps its files

Projects, the autosave and the kit library live on the SD card under `/ENAGA/DATA/STRATA/`, and
resampled takes go to `/RECORD`. Samples come from `/SAMPLES`, and the sample browser also reads
the stock SmplTrek folders. [MANUAL.md](MANUAL.md), "On the card", has the full layout.

## Going back

Install an official SmplTrek system update the same way as step 1 (its `SmplTrek.bin` on the
card's root, HOME held at power-on, SYSTEM UPDATE). Neither Enaga nor STRATA writes the SPI
flash or touches the bootloader, so that restores the unit completely. Projects stay on the card
under `/ENAGA/DATA/`.
