#ifndef FFC_SHADING_MATH_H
#define FFC_SHADING_MATH_H

#include <stdint.h>
#include <stddef.h>

/* C = (open - mean(open)) - (shutter - mean(shutter)).
 * Compute in signed wide arithmetic; preserve the relative DC level. */
static inline int ffc_shading_build(const uint16_t *open_raw,
    const uint16_t *shutter_raw, int16_t *correction, size_t count)
{
    size_t i;
    int64_t sum = 0;
    if (!open_raw || !shutter_raw || !correction || !count) return -1;
    for (i = 0; i < count; ++i)
        sum += (int32_t)open_raw[i] - (int32_t)shutter_raw[i];
    for (i = 0; i < count; ++i) {
        int64_t n = ((int64_t)open_raw[i] - shutter_raw[i]) * (int64_t)count - sum;
        int64_t c = n >= 0 ? (n + (int64_t)count / 2) / (int64_t)count :
            -((-n + (int64_t)count / 2) / (int64_t)count);
        if (c < -32768 || c > 32767) return -1;
        correction[i] = (int16_t)c;
    }
    return 0;
}

#endif
