#ifndef RAW_DENOISE_INTERNAL_H
#define RAW_DENOISE_INTERNAL_H

#include "raw_temporal_denoise.h"
#include "Temporal/raw_temporal_kernel.h"

typedef void RAW_FRAME_KERNEL(RAW_TEMPORAL_DENOISE_STATE *s, const uint16_t *input,
    size_t input_stride, uint16_t *output, size_t output_stride,
    uint32_t width, uint32_t height, const RAW_KERNEL_PARAMS *p);

RAW_FRAME_KERNEL raw_legacy, raw_dual, raw_init, raw_dual_init;
RAW_FRAME_KERNEL raw_spatial_legacy, raw_spatial_dual, raw_spatial_init, raw_spatial_dual_init;
RAW_FRAME_KERNEL raw_selective_legacy, raw_selective_dual, raw_selective_init, raw_selective_dual_init;
RAW_FRAME_KERNEL raw_selective_dual_separate;

/* Constant mode flags let both modules inline the same temporal operation. */
#define RAW_APPLY_PIXEL(DUAL, RESET) do { \
    uint32_t value; \
    if (RESET) { \
        history[x] = current; \
        if (DUAL) { slow[x] = current; motion[x] = 0; } \
        value = current; \
    } else if (DUAL) { \
        value = raw_dual_speed(current, center << 8, \
            &history[x], &slow[x], &motion[x], p); \
    } else { \
        uint32_t previous = history[x]; \
        value = raw_difference(current, previous) <= p->threshold ? \
            (previous * p->weight + current + p->rounding) >> p->shift : current; \
        history[x] = value; \
    } \
    row[x] = (uint16_t)((value + 128U) >> 8); \
} while (0)

#endif
