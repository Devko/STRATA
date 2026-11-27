/* Hints for TI's software pipeliner, and the switch that turns them on.
 *
 * The C674x has no packed-float instructions, so "vector code" on this core means software
 * pipelining: cl6x overlaps successive iterations of a branch-free loop across the eight
 * functional units. What helps it is knowing facts it cannot prove -- that pointers do not alias
 * (restrict, used everywhere already), and how many times a loop runs. MUST_ITERATE states the
 * second, and a wrong statement is wrong code, not a slow loop, so the hints are OFF until a
 * cl6x build has been measured with them (tools/ti_ab.sh, docs/BUDGET.md "The cl6x vectorisation
 * pass"). They cost nothing in a GCC build either way: the macros expand to nothing there.
 *
 * STRATA_BLOCK_LOOP goes on the line before a loop over a block or a slice, `for (i = 0; i < n;
 * i++)`, whose function has already returned for n <= 0. It promises cl6x one iteration at
 * least, which lets it drop the zero-trip test and the guard around the pipelined loop. That is
 * the only fact every such loop has: slices are cut at trigs, gates and control steps, so no
 * larger minimum and no multiple holds in general. */
#ifndef STRATA_TIHINTS_H
#define STRATA_TIHINTS_H

#ifndef STRATA_TI_HINTS
#define STRATA_TI_HINTS 0
#endif

/* The VA voice's fused back half (valoops.h, strata_va_back). ON since 2026-11-27. It was off from
 * 2026-11-22, when an emulator A/B found its cl6x render "drifting" from the unfused path's. That
 * was the same audio a few blocks apart: a cheaper callback lets the scenario's scripted keys act
 * sooner (docs/BUDGET.md, "The HALL in passes"). Checked in the app instead, cl6x build on the
 * emulator: the three passes on copies beside the fused loop, 16,233 slices of scenario_m3 and
 * scenario_factory, every output sample and every filter and envelope state equal. The host test
 * va_the_fused_back_half_is_bit_exact_against_filter_envelope_and_tail holds it there. */
#ifndef STRATA_VA_FUSED
#define STRATA_VA_FUSED 1
#endif

#if defined(__TI_COMPILER_VERSION__) && STRATA_TI_HINTS
#define STRATA_BLOCK_LOOP _Pragma("MUST_ITERATE(1)")
#else
#define STRATA_BLOCK_LOOP
#endif

/* Packed 16-bit arithmetic, the SIMD the C674x does have (from the C64x+): DOTP2 multiplies two
 * pairs of 16-bit halves and adds the products in one instruction. A stereo int16 frame is such
 * a pair, so one unaligned 32-bit load and a DOTP2 by (1, 1) give L + R where the plain C needs
 * two 16-bit loads and an add. The sample readers are load-bound (two .D units, and a stereo
 * Hermite read takes eight taps a sample), so halving their loads is the point. The sum is exact
 * in integers either way, so this is bit-exact by arithmetic -- but the host cannot run it, which
 * is why it waits behind STRATA_TI_PACKED for tonight's cl6x render and measurement. */
#ifndef STRATA_TI_PACKED
#define STRATA_TI_PACKED 0
#endif

#if defined(__TI_COMPILER_VERSION__) && STRATA_TI_PACKED
#include <c6x.h>
#define STRATA_PAIR_SUM(p) ((int32_t)_dotp2((int)_mem4_const(p), 0x00010001))
#else
#define STRATA_PAIR_SUM(p) ((int32_t)(p)[0] + (int32_t)(p)[1])
#endif

#endif
