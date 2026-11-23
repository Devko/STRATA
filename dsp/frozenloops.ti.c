/* A frozen track's read loop (frozenloops.h). Built by cl6x when it is installed, else GCC. */
#include "frozenloops.h"

#include "tihints.h"

void strata_frozen_add(float *restrict dst, const int16_t *restrict src, int n, float gain,
                       float step) {
    int i;
    STRATA_BLOCK_LOOP
    for (i = 0; i < n; i++) {
        dst[i] += (float)src[i] * (gain + step * (float)i);
    }
}
