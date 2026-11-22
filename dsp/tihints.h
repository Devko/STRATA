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

/* The VA voice's fused back half (valoops.h, strata_va_back). OFF: it is bit-exact on the host
 * and with GCC for the device, and saves ~1.7 points with cl6x, but on 2026-11-22 the cl6x
 * build rendered audibly different audio with it (docs/BUDGET.md, "The cl6x vectorisation
 * pass"), and until that is explained the three separate passes, which are what the unit has
 * always run, stay in. tools/ti_ab.sh's `fused` variant builds it with 1. */
#ifndef STRATA_VA_FUSED
#define STRATA_VA_FUSED 0
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
