# STRATA: rules for Claude Code

- Read docs/SPEC.md, docs/REVIEW.md and docs/PLAN.md before any change, and follow the
  milestone order in PLAN.md. Ask before changing a decision they record.
- Target: an Enaga app on the Sonicware SmplTrek (TI C6748, 372 MHz). Enaga is pinned at
  1b41111. Read third_party/enaga/docs/api.md for every API you use. Never invent an Enaga
  API name: check the header.
- Build in WSL, not Windows. `source tools/env.sh` first. There is no cmake, and no sudo
  either, so do not add a dependency that needs either one.
- Do not write a master clock. Enaga owns it: <enaga/clock.hpp>, 96 PPQN, and
  clock::ticks() in audio() gives each tick's frame in the block. STRATA schedules steps on
  those ticks and owns swing, speed, length and micro-timing only.
- STRATA does not own the tempo. CLOCK SRC is the user's setting; start/stop/set_tempo
  return false under external sync. Always handle the refusal and follow on_transport.
- audio(): no allocation, blocking, files, drawing or LEDs. Buffers are interleaved stereo
  (L R L R). Cross-context state only through single-writer atomics or SPSC rings.
- Allocate all voices, buffers and pools in init(). Save state in stop() — destructors
  never run. No std::vector, std::string or iostreams. No libm: use <enaga/dsp.hpp> math.
- C++20, -fno-exceptions -fno-rtti, -Wall -Wextra -Werror. Stack frames at most 16 KiB,
  no VLAs or alloca.
- Never cast file bytes to structs: serialize byte by byte. No float division in hot loops.
- Hot inner loops go in *.ti.c files: plain C, branch-free (use ?:), building with both GCC
  and TI cl6x. cl6x is worth about 2.2x and the 60% target depends on it.
- build_app.py compiles one flat folder with one include path. Every filename must be
  unique across core/, dsp/ and app/, and includes are bare ("pattern.hpp").
  tools/build.sh enforces the uniqueness; do not work around it.
- core/ never includes Enaga headers. Every core change comes with a host test
  (`make -C host test`).
- Every feature: host test, then an emulator scenario, then a line in docs/UNIT_CHECKLIST.md.
- After any DSP change, run the load scenario and update docs/BUDGET.md with figures both
  with and without cl6x. Worst case must stay under 60%. Cut effects or insert stages before
  cutting voices: all 18 voices together are about 11% of the block.
- Never modify third_party/enaga. Enaga bugs are reported upstream with a failing case.
- Never write the SPI flash. Never produce update files with BOOT or MCU sections.
- Small commits, one concern each, each with its test.
