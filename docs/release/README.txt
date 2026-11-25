STRATA {RELEASE} for the Sonicware SmplTrek
========================================

An 8-track techno performance groovebox, built as an app for Enaga, the replacement
firmware for the SmplTrek. This is an early release: read "Before you install" first.

https://github.com/Devko/STRATA

Requires Enaga {ENAGA} on the unit, exactly. STRATA app version {APP_VERSION}.
Enaga is not in this zip: get it from Enaga's own release (step 1 below).


AT YOUR OWN RISK
----------------
Installing third-party firmware is at your own risk. Neither Enaga nor STRATA is made,
supported or endorsed by Sonicware, and installing them may affect your warranty.
STRATA is provided as is, without warranty of any kind (see LICENSE.txt). Please do not
contact Sonicware support about STRATA or Enaga.

Sonicware and SmplTrek are trademarks of their respective owners, used here only to say
which hardware STRATA runs on.


WHAT IS IN THIS ZIP
-------------------
  SD-CARD/ENAGA/APPS/STRATA/APP.ENA      the STRATA app
  README.txt, CHANGELOG.md, LICENSE.txt, THIRD_PARTY_NOTICES.md
  SHA256SUMS.txt                         checksums of the files under SD-CARD/


BEFORE YOU INSTALL
------------------
Do not skip these: a hung image leaves the power key dead.

  [ ] SYSTEM INFO shows BOOT 1.0.17. All the recovery evidence is for that bootloader.
  [ ] A second SD card holds an official SmplTrek.bin from Sonicware, so you can restore
      the unit.
  [ ] Holding HOME (F4) at power-on reaches BOOT MODE.
  [ ] The unit runs on the power adapter WITHOUT batteries, so a hung image can be
      power-cycled.

This release is for the SmplTrek only. The Lofi-12 XT and CyDrums are the same board, but
STRATA has not run on either.


INSTALLING
----------
1. Install Enaga {ENAGA}, from Enaga's own release:

     {ENAGA_URL}

   Download the SmplTrek zip there and follow Enaga's install steps: copy its
   SmplTrek.bin to the ROOT of the SD card, hold HOME (F4) while switching on to reach
   BOOT MODE, and choose SYSTEM UPDATE. Already on Enaga {ENAGA}? Skip this step.

2. Copy STRATA to the card, keeping the folders:

     /ENAGA/APPS/STRATA/APP.ENA

3. Switch on. Enaga starts the last app that ran, or the only one installed.
   F1 + POWER opens the Enaga menu, which lists the apps and switches between them.
   In STRATA, hold FUNC to see what every key does, and FUNC + HOME then OK saves.

A first start opens the factory project: five patterns in bank A (A01 GROOVE, A02 GROOVE+,
A03 BREAK, A04 PEAK, A05 PAD) at 130 BPM in C minor. Press PLAY.

Projects are saved on the card under /ENAGA/DATA/STRATA/.


GOING BACK
----------
Install an official SmplTrek system update the same way (BOOT MODE, SYSTEM UPDATE).
Neither Enaga nor STRATA writes the SPI flash or touches the bootloader, so that restores
the unit completely. Your projects stay on the card.


MORE
----
CHANGELOG.md lists what is in this release and its known limits. In the repository,
docs/MANUAL.md is the user manual, docs/CONTROLS.md explains how the controls are laid
out, and docs/CHEATSHEET.md has every control on one page:

  https://github.com/Devko/STRATA/blob/{RELEASE}/docs/MANUAL.md
  https://github.com/Devko/STRATA/blob/{RELEASE}/docs/CHEATSHEET.md
  https://github.com/Devko/STRATA/blob/{RELEASE}/docs/CONTROLS.md

Put your own WAV files in /SAMPLES/ on the card; the sample browser opens there.
