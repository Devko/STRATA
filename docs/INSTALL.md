# Installing STRATA on a SmplTrek

> [!CAUTION]
> Installing third-party firmware is **at your own risk**. Neither Enaga nor STRATA is made,
> supported or endorsed by Sonicware, and installing them may affect your warranty. Keep an
> official update file at hand so you can restore the unit.

STRATA runs on [Enaga](https://github.com/seawaffle/enaga), so the unit needs Enaga's firmware
first and then the STRATA app on the SD card. **The easy way is a release zip** from
[Releases](https://github.com/Devko/STRATA/releases): it holds both, laid out for the card
(the matching Enaga `SmplTrek.bin` and `ENAGA/APPS/STRATA/APP.ENA`), with these steps in its
README.txt. Releases are pre-releases until 1.0. To build `APP.ENA` yourself, see
[BUILDING.md](BUILDING.md).

## Before you install

From the safety section of [UNIT_CHECKLIST.md](UNIT_CHECKLIST.md). Do not skip these: a hung
image leaves the power key dead.

- [ ] SYSTEM INFO shows **BOOT 1.0.17**. All the recovery evidence is for that bootloader.
- [ ] A second SD card holds an official `SmplTrek.bin`, so you can restore the unit.
- [ ] Holding **HOME** at power-on reaches BOOT MODE.
- [ ] The unit runs on the power adapter **without batteries**, so a hung image can be
      power-cycled.

## 1. Install the matching Enaga

Each STRATA build is made against one Enaga release, and the firmware on the unit must match it:
Enaga's loader checks only an ABI number, so a mismatch loads and then misbehaves.

| STRATA | Enaga firmware |
| --- | --- |
| v0.1.0 to v0.3.1 (the current release) | [v0.2.0](https://github.com/seawaffle/enaga/releases/tag/v0.2.0) |
| a build from `main`, and the next release | [v0.3.0](https://github.com/seawaffle/enaga/releases/tag/v0.3.0) |

1. Take `SD-CARD/SmplTrek.bin` from the STRATA release zip (byte for byte the file in Enaga's
   own zip), or download the SmplTrek zip from the Enaga release in the table.
2. Copy it to the **root** of the SD card as `SmplTrek.bin` (Enaga's README, "Installing", has
   the details).
3. Hold **HOME** while switching on to reach BOOT MODE, and choose **SYSTEM UPDATE**.

## 2. Copy STRATA to the card

Copy the release zip's `SD-CARD/ENAGA/APPS/STRATA/APP.ENA` (or your own build) to:

```text
/ENAGA/APPS/STRATA/APP.ENA
```

Enaga starts the last app that ran, or the only one installed. **FUNC + POWER** opens the Enaga
menu, which lists the apps and switches between them.

## Updating

Updating from an earlier STRATA release is step 2 only while the Enaga version in the table
above stays the same; when it changes (v0.3.1 to the next release: 0.2.0 to 0.3.0), do step 1
first. Your projects stay on the card. Before 1.0 a release may change the project format; the
[changelog](../CHANGELOG.md) says when older projects play differently.

## Where STRATA keeps its files

Projects, the autosave and the kit library live on the SD card under `/ENAGA/DATA/STRATA/`, and
resampled takes go to `/RECORD`. Samples come from `/SAMPLES`, and the sample browser also reads
the stock SmplTrek folders. [MANUAL.md](MANUAL.md), "On the card", has the full layout.

## Going back

Install an official SmplTrek system update the same way as step 1. Neither Enaga nor STRATA
writes the SPI flash or touches the bootloader, so that restores the unit completely. Projects
stay on the card under `/ENAGA/DATA/`.
