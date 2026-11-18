#ifndef RAW_TEMPORAL_KERNEL_H
#define RAW_TEMPORAL_KERNEL_H

#include <stdint.h>

typedef struct {
    uint32_t threshold, shift, weight, rounding, spatial_threshold;
    uint32_t low, high, hysteresis, hold;
} RAW_KERNEL_PARAMS;

static inline uint32_t raw_difference(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static inline uint32_t raw_iir(uint32_t current, uint32_t previous, uint32_t shift)
{
    return (previous * ((1U << shift) - 1U) + current + (1U << (shift - 1U))) >> shift;
}

static inline uint32_t raw_dual_speed(uint32_t current, uint32_t original,
    uint32_t *fast, uint32_t *slow, uint16_t *motion,
    const RAW_KERNEL_PARAMS *p)
{
    uint32_t jump = raw_difference(current, *fast);
    uint32_t f = raw_iir(current, *fast, 1U);
    uint32_t s = raw_iir(current, *slow, 4U);
    uint32_t difference = raw_difference(f, s);
    uint32_t previous_mode = *motion >> 8, hold = *motion & 255U;
    uint32_t low = p->low;
    uint32_t high = p->high;
    uint32_t hysteresis = p->hysteresis;
    uint32_t mode;
    if (previous_mode == 2U) high -= hysteresis;
    if (previous_mode != 0U) low -= hysteresis;
    mode = difference >= high || jump >= high ? 2U : difference >= low ? 1U : 0U;
    if (mode && mode >= previous_mode) {
        hold = p->hold;
    } else if (mode < previous_mode && hold) {
        mode = previous_mode;
        --hold;
    } else {
        hold = mode ? p->hold : 0U;
    }
    /* Refresh Slow throughout motion; the Fast residual detects new steps. */
    if (mode == 2U) f = s = current;
    else if (mode == 1U) s = f;
    *fast = f;
    *slow = s;
    *motion = (uint16_t)((mode << 8) | hold);
    return mode == 2U ? original : mode == 1U ? f : s;
}

#endif
