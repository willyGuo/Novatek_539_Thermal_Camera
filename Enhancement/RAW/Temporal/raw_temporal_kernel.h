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
    uint32_t previous_fast = *fast, state = *motion;
    uint32_t previous_mode = state >> 8, hold = state & 255U;
    uint32_t high = p->high - (previous_mode == 2U ? p->hysteresis : 0U);
    uint32_t f, s, difference, mode;

    /* A strong jump chooses RAW and overwrites both histories regardless of IIR. */
    if (raw_difference(current, previous_fast) >= high) {
        *fast = *slow = current;
        *motion = (uint16_t)(0x200U | p->hold);
        return original;
    }

    f = raw_iir(current, previous_fast, 1U);
    s = raw_iir(current, *slow, 4U);
    difference = raw_difference(f, s);
    mode = difference >= high ? 2U :
        difference >= p->low - (previous_mode != 0U ? p->hysteresis : 0U) ? 1U : 0U;
    if (mode < previous_mode && hold) {
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
