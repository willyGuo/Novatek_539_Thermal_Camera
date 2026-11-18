#include "../raw_denoise_internal.h"
#include <string.h>
#if defined(__ARM_NEON) && !defined(RAW_TEMPORAL_DISABLE_NEON)
#include <arm_neon.h>
#define RAW_TEMPORAL_USE_NEON 1
#else
#define RAW_TEMPORAL_USE_NEON 0
#endif

static inline uint32_t raw_selective_q8(uint32_t center, uint32_t north,
    uint32_t south, uint32_t east, uint32_t west, uint32_t threshold)
{
    north = raw_difference(center, north) <= threshold ? north : center;
    south = raw_difference(center, south) <= threshold ? south : center;
    east = raw_difference(center, east) <= threshold ? east : center;
    west = raw_difference(center, west) <= threshold ? west : center;
    return (4U * center + north + south + east + west) << 5;
}

/* Fused kernels read original neighbors and immediately apply the temporal step. */
#define RAW_DEFINE_SPATIAL_KERNEL(NAME, SELECTIVE, DUAL, RESET) \
void NAME(RAW_TEMPORAL_DENOISE_STATE *s, const uint16_t *input, size_t input_stride, \
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height, const RAW_KERNEL_PARAMS *p) \
{ \
    uint32_t y, x; \
    for (y = 0; y < height; ++y) { \
        const uint16_t *source = (const uint16_t *)((const uint8_t *)input + y * input_stride); \
        uint16_t *row = (uint16_t *)((uint8_t *)output + y * output_stride); \
        uint32_t *history = s->history_q8 + (size_t)y * width; \
        uint32_t *slow = (DUAL) ? s->slow_history_q8 + (size_t)y * width : NULL; \
        uint16_t *motion = (DUAL) ? s->motion + (size_t)y * width : NULL; \
        if (y && y + 1U < height) { \
            const uint16_t *south = (const uint16_t *)((const uint8_t *)source + input_stride); \
            uint32_t west = source[0]; \
            uint32_t center = west, current = center << 8; \
            s->spatial_row[0] = (uint16_t)center; \
            x = 0; \
            RAW_APPLY_PIXEL(DUAL, RESET); \
            for (x = 1; x < width - 1U; ++x) { \
                uint32_t north = s->spatial_row[x], east = source[x + 1U], down = south[x]; \
                center = source[x]; current = center << 8; \
                if (SELECTIVE) \
                    current = raw_selective_q8(center, north, down, east, west, p->spatial_threshold); \
                else if (raw_difference(center, north) <= p->spatial_threshold && \
                    raw_difference(center, down) <= p->spatial_threshold && \
                    raw_difference(center, east) <= p->spatial_threshold && \
                    raw_difference(center, west) <= p->spatial_threshold) \
                    current = (4U * center + north + down + east + west) << 5; \
                s->spatial_row[x] = (uint16_t)center; \
                west = center; \
                RAW_APPLY_PIXEL(DUAL, RESET); \
            } \
            center = source[x]; current = center << 8; \
            s->spatial_row[x] = (uint16_t)center; \
            RAW_APPLY_PIXEL(DUAL, RESET); \
        } else { \
            for (x = 0; x < width; ++x) { \
                uint32_t center = source[x], current = center << 8; \
                s->spatial_row[x] = (uint16_t)center; \
                RAW_APPLY_PIXEL(DUAL, RESET); \
            } \
        } \
    } \
}

RAW_DEFINE_SPATIAL_KERNEL(raw_spatial_legacy,       0, 0, 0)
RAW_DEFINE_SPATIAL_KERNEL(raw_spatial_dual,         0, 1, 0)
RAW_DEFINE_SPATIAL_KERNEL(raw_spatial_init,         0, 0, 1)
RAW_DEFINE_SPATIAL_KERNEL(raw_spatial_dual_init,    0, 1, 1)
RAW_DEFINE_SPATIAL_KERNEL(raw_selective_legacy,     1, 0, 0)
RAW_DEFINE_SPATIAL_KERNEL(raw_selective_dual,       1, 1, 0)
RAW_DEFINE_SPATIAL_KERNEL(raw_selective_init,       1, 0, 1)
RAW_DEFINE_SPATIAL_KERNEL(raw_selective_dual_init,  1, 1, 1)
#undef RAW_DEFINE_SPATIAL_KERNEL

#if RAW_TEMPORAL_USE_NEON
static inline uint16x8_t raw_select_neighbor(uint16x8_t center, uint16x8_t neighbor,
    uint16x8_t threshold)
{
    return vbslq_u16(vcleq_u16(vabdq_u16(center, neighbor), threshold), neighbor, center);
}

static inline uint32x4_t raw_selective_q8_half(uint16x4_t center, uint16x4_t north,
    uint16x4_t south, uint16x4_t east, uint16x4_t west)
{
    /* Widen before summing to preserve corrected RAW16 through 65535 DN. */
    uint32x4_t neighbors = vaddq_u32(vaddl_u16(north, south), vaddl_u16(east, west));
    return vshlq_n_u32(vaddq_u32(vshll_n_u16(center, 2), neighbors), 5);
}

static inline void raw_selective_q8_neon(const uint16_t *center, const uint16_t *north,
    const uint16_t *south, uint16x8_t threshold, uint32_t q8[8])
{
    uint16x8_t c = vld1q_u16(center);
    uint16x8_t n = raw_select_neighbor(c, vld1q_u16(north), threshold);
    uint16x8_t s = raw_select_neighbor(c, vld1q_u16(south), threshold);
    uint16x8_t e = raw_select_neighbor(c, vld1q_u16(center + 1), threshold);
    uint16x8_t w = raw_select_neighbor(c, vld1q_u16(center - 1), threshold);
    vst1q_u32(q8, raw_selective_q8_half(vget_low_u16(c), vget_low_u16(n),
        vget_low_u16(s), vget_low_u16(e), vget_low_u16(w)));
    vst1q_u32(q8 + 4, raw_selective_q8_half(vget_high_u16(c), vget_high_u16(n),
        vget_high_u16(s), vget_high_u16(e), vget_high_u16(w)));
}

static inline uint16_t raw_dual_pixel(uint32_t current, uint16_t original,
    uint32_t *fast, uint32_t *slow, uint16_t *motion, const RAW_KERNEL_PARAMS *p)
{
    uint32_t value = raw_dual_speed(current, (uint32_t)original << 8, fast, slow, motion, p);
    return (uint16_t)((value + 128U) >> 8);
}

/* Steady-state, separate source/output only. Neighbors are read from the
 * original image, and each batch feeds the existing scalar temporal filter. */
static void raw_selective_dual_neon(RAW_TEMPORAL_DENOISE_STATE *s,
    const uint16_t *input, size_t input_stride, uint16_t *output, size_t output_stride,
    uint32_t width, uint32_t height, const RAW_KERNEL_PARAMS *p)
{
    uint16x8_t threshold = vdupq_n_u16((uint16_t)p->spatial_threshold);
    uint32_t y;
    for (y = 0; y < height; ++y) {
        const uint16_t *source = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *row = (uint16_t *)((uint8_t *)output + y * output_stride);
        size_t offset = (size_t)y * width;
        uint32_t *fast = s->history_q8 + offset;
        uint32_t *slow = s->slow_history_q8 + offset;
        uint16_t *motion = s->motion + offset;
        uint32_t x = 0;

        if (y > 0 && y + 1U < height) {
            const uint16_t *north = (const uint16_t *)((const uint8_t *)source - input_stride);
            const uint16_t *south = (const uint16_t *)((const uint8_t *)source + input_stride);
            row[0] = raw_dual_pixel((uint32_t)source[0] << 8, source[0], fast, slow, motion, p);
            /* The east load of the final lane must still be within the row. */
            for (x = 1; width - x > 8U; x += 8U) {
                uint32_t q8[8];
                unsigned int lane;
                raw_selective_q8_neon(source + x, north + x, south + x, threshold, q8);
                for (lane = 0; lane < 8U; ++lane) {
                    uint32_t pixel = x + lane;
                    row[pixel] = raw_dual_pixel(q8[lane], source[pixel], fast + pixel,
                        slow + pixel, motion + pixel, p);
                }
            }
            for (; x < width - 1U; ++x) {
                uint32_t q8 = raw_selective_q8(source[x], north[x], south[x], source[x + 1U],
                    source[x - 1U], p->spatial_threshold);
                row[x] = raw_dual_pixel(q8, source[x], fast + x, slow + x, motion + x, p);
            }
        }
        for (; x < width; ++x)
            row[x] = raw_dual_pixel((uint32_t)source[x] << 8, source[x], fast + x, slow + x, motion + x, p);
    }
    /* Match the scalar path's row scratch after processing the last row. */
    memcpy(s->spatial_row, (const uint8_t *)input + (height - 1U) * input_stride,
        (size_t)width * sizeof(*input));
}
#endif

void raw_selective_dual_separate(RAW_TEMPORAL_DENOISE_STATE *s,
    const uint16_t *input, size_t input_stride, uint16_t *output, size_t output_stride,
    uint32_t width, uint32_t height, const RAW_KERNEL_PARAMS *p)
{
#if RAW_TEMPORAL_USE_NEON
    if (input != output && width >= 10U) {
        raw_selective_dual_neon(s, input, input_stride, output, output_stride, width, height, p);
        return;
    }
#endif
    raw_selective_dual(s, input, input_stride, output, output_stride, width, height, p);
}
