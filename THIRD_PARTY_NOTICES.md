# Third-party notices

STRATA itself is MIT licensed (`LICENSE`). This file carries the notices for the work it is
built on and the work it learned from. Both are MIT, so the whole thing is MIT: there is no
copyleft anywhere in the dependency set, and nothing here restricts what STRATA may be used for.

A note on why the second entry is here at all: STRATA **reimplements** published drum-synthesis
topologies rather than porting anyone's code, so it is arguable that no notice is owed for it.
Reproducing it costs nothing and removes the argument, which is the better trade.

---

## Enaga

The app platform STRATA runs on. Vendored as a git submodule at `third_party/enaga`, pinned at
release `v0.2.0` (commit `3da7f89`), and **never modified** (`CLAUDE.md`). Its headers and its DSP library are
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

## If the toolchain's output is ever redistributed

Separate from everything above, and still open: `docs/BUDGET.md` records that TI's `cl6x` is
load-bearing for the CPU target, and whether TI's licence permits redistributing `cl6x`-compiled
output inside a released `APP.ENA` has not been read by anyone. That is a question about TI's
terms, not about any licence in this file.
