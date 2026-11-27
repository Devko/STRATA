# Third-party notices

STRATA itself is MIT licensed (`LICENSE`). This file carries the notices for the work it is
built on and the work it learned from. Both are MIT, so the whole thing is MIT: there is no
copyleft anywhere in the dependency set, and nothing here restricts what STRATA may be used for.

A note on why the second entry is here at all: STRATA **reimplements** published drum-synthesis
topologies rather than porting anyone's code, so it is arguable that no notice is owed for it.
Reproducing it costs nothing and removes the argument, which is the better trade.

Sonicware, SmplTrek, Lofi-12 XT and CyDrums are trademarks of their respective owners. STRATA is
not affiliated with or endorsed by Sonicware, and no Sonicware code, firmware or sounds are part
of it.

---

## Enaga

The app platform STRATA runs on. Vendored as a git submodule at `third_party/enaga`, pinned at
release `v0.4.0` (commit `7487bbf`), and **never modified** (`CLAUDE.md`). Its headers and its DSP library are
compiled into a released `APP.ENA`, so a release distributes Enaga's code as well as STRATA's.

<https://github.com/seawaffle/enaga>

```
MIT License

Copyright (c) 2026 seawaffle

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Enaga's firmware is not in a release zip

From the release after v0.3.1 on, a STRATA release zip (`tools/package_release.py`) carries only
the STRATA app. Users install Enaga's SmplTrek update file from Enaga's own release, which the
zip's README.txt links; the packaging script refuses a zip with any `.bin` in it.

v0.1.0 to v0.3.1 did carry Enaga 0.2.0's `SmplTrek.bin`. That was permitted: checked 2026-11-22
against the source, the image is built only from code under these licences, and Enaga's README
states it "contains no Sonicware code". For those zips:

| In `SmplTrek.bin` | Licence | What redistributing a binary asks |
| --- | --- | --- |
| Enaga (`firmware/`, `loader/`) | MIT, notice above | include the notice: this file, in the zip |
| FatFs R0.15a (`firmware/third_party/fatfs`), © 2024 ChaN | 1-clause BSD-style | nothing for a binary; the notice is below anyway |
| TinyUSB, a subset (`firmware/third_party/tinyusb`), © hathach and contributors | MIT | include the notice: below |
| `libgcc` | GPL-3 with the GCC Runtime Library Exception | nothing: the exception permits it |

An official Sonicware update file, whose firmware must never be redistributed, cannot end up
in a zip either: the script takes `APP.ENA` alone.

#### TinyUSB

<https://github.com/hathach/tinyusb>

```
The MIT License (MIT)

Copyright (c) 2012-2026, hathach (tinyusb.org)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

#### FatFs

<http://elm-chan.org/fsw/ff/>

```
FatFs - Generic FAT Filesystem Module  R0.15a
Copyright (C) 2024, ChaN, all right reserved.

FatFs module is an open source software. Redistribution and use of FatFs in
source and binary forms, with or without modification, are permitted provided
that the following condition is met:

1. Redistributions of source code must retain the above copyright notice,
   this condition and the following disclaimer.

This software is provided by the copyright holder and contributors "AS IS"
and any warranties related to this software are DISCLAIMED.
The copyright owner or contributors be NOT LIABLE for any damages caused
by use of this software.
```

---

## Mutable Instruments Plaits

The reference for STRATA's drum synthesis. `docs/SOUND.md` records what was taken and why; the
headers of `dsp/kick.*`, `dsp/snare.*`, `dsp/hat.*` and `dsp/perc.*` each name the specific model
and the constants it published.

**What was taken:** the model *topologies* and the published numeric constants — the 909 snare's
1.47 oscillator ratio and its 40–70 ms noise hold, the 808 hat's six square-wave ratios at a
nominal 414 Hz, the 909 kick's 1.3 ms phase hold at 0.25 and its `20 ms × 2^(5·decay²)`
amplitude envelope, and the clean-sine-to-rational-fold blend its author calls dirtiness.

**What was not taken:** the code. STRATA's machines are written against Enaga's primitives with
block processing and control-rate coefficients, which is a different shape from `stmlib`'s
per-sample classes; no file here is a port, a translation or a transcription of a Plaits source
file, and `stmlib` is not vendored, linked or compiled into anything STRATA builds.

<https://github.com/pichenettes/eurorack>

```
Copyright 2016 Emilie Gillet.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Not dependencies, and deliberately so

Kept out so that STRATA can stay MIT. Any of these would make a release GPL-3, which would in
turn bind anyone building on STRATA:

- **Surge XT**, **VCV Rack's Fundamental modules** and most open plugin code — GPL-3.
- The **Dirtywave M8**'s firmware is not open source at all: what its repositories publish is a
  compiled binary for Teensy plus documentation, and the lively open projects around it (`m8c`
  and its forks) are *clients* that draw its screen over USB rather than implementations of its
  sound engine. There is nothing there to read even with a licence that allowed it.

## If the toolchain's output is redistributed

Separate from everything above: an `APP.ENA` built with TI's `cl6x` contains no TI code, headers
or run-time library, only instruction sequences cl6x chose from STRATA's and Enaga's source
(`docs/BUDGET.md`, "The cl6x licence question, examined"). Compiler licences restrict the tools,
not their output. What is still owed before the first release built with cl6x is the ten-minute
read of the licence text TI's installer put in `~/ti/ti-cgt-c6000_8.5.0.LTS`, to confirm no
condition is attached to output (`docs/release/RELEASING.md`). That is a question about TI's
terms, not about any licence in this file.
