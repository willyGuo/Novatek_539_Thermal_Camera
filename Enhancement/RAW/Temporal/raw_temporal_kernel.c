#include "../raw_denoise_internal.h"

#define RAW_DEFINE_TEMPORAL_KERNEL(NAME, DUAL, RESET) \
void NAME(RAW_TEMPORAL_DENOISE_STATE *s, const uint16_t *input, size_t input_stride, \
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height, const RAW_KERNEL_PARAMS *p) \
{ \
    uint32_t y, x; \
    for (y = 0; y < height; ++y) { \
        const uint16_t *source = (const uint16_t *)((const uint8_t *)input + y * input_stride); \
        uint16_t *row = (uint16_t *)((uint8_t *)output + y * output_stride); \
        size_t offset = (size_t)y * width; \
        uint32_t *history = s->history_q8 + offset; \
        uint32_t *slow = (DUAL) ? s->slow_history_q8 + offset : NULL; \
        uint16_t *motion = (DUAL) ? s->motion + offset : NULL; \
        for (x = 0; x < width; ++x) { \
            uint32_t center = source[x], current = center << 8; \
            RAW_APPLY_PIXEL(DUAL, RESET); \
        } \
    } \
}

RAW_DEFINE_TEMPORAL_KERNEL(raw_legacy,    0, 0)
RAW_DEFINE_TEMPORAL_KERNEL(raw_dual,      1, 0)
RAW_DEFINE_TEMPORAL_KERNEL(raw_init,      0, 1)
RAW_DEFINE_TEMPORAL_KERNEL(raw_dual_init, 1, 1)
