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

#if defined(__TI_COMPILER_VERSION__) && STRATA_TI_HINTS
#define STRATA_BLOCK_LOOP _Pragma("MUST_ITERATE(1)")
#else
#define STRATA_BLOCK_LOOP
#endif

#endif
