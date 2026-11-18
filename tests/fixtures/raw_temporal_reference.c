/* Frozen 2026-11-18 pre-optimization oracle for test_raw_temporal_parity.py.
 * Keep independent of optimized kernels; deliberate algorithm changes require
 * reviewing the expected behavior rather than copying optimized code here. */
#include "raw_temporal_denoise.h"
#include <stdlib.h>
#include <string.h>
#if defined(__LINUX)
#include <pthread.h>
#else
#include <FreeRTOS_POSIX.h>
#include <FreeRTOS_POSIX/pthread.h>
#endif

static RAW_TEMPORAL_DENOISE_PARAMS settings = RAW_TEMPORAL_DENOISE_DEFAULTS;
static pthread_mutex_t settings_lock = PTHREAD_MUTEX_INITIALIZER;

int raw_temporal_denoise_validate(const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    return !p || p->enable > 1U || !p->threshold_dn || p->threshold_dn > 16383U ||
        !p->history_shift || p->history_shift > 3U ||
        p->spatial_enable > 1U || p->spatial_threshold_dn > 16383U ||
        p->dual_speed_enable > 1U || p->motion_threshold_dn > 16383U ||
        p->motion_strong_threshold_dn > 16383U ||
        p->motion_hysteresis_dn > 16383U || p->motion_hold_frames > 255U ||
        (p->dual_speed_enable && (!p->motion_threshold_dn ||
            p->motion_strong_threshold_dn <= p->motion_threshold_dn ||
            p->motion_hysteresis_dn >= p->motion_threshold_dn)) ? -1 : 0;
}
void raw_temporal_denoise_get_params(RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    if (!p) return;
    pthread_mutex_lock(&settings_lock);
    *p = settings;
    pthread_mutex_unlock(&settings_lock);
}
int raw_temporal_denoise_set_params(const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    if (raw_temporal_denoise_validate(p)) return -1;
    pthread_mutex_lock(&settings_lock);
    settings = *p;
    pthread_mutex_unlock(&settings_lock);
    return 0;
}
void raw_temporal_denoise_reset(RAW_TEMPORAL_DENOISE_STATE *s)
{
    if (s) s->valid = 0;
}
void raw_temporal_denoise_destroy(RAW_TEMPORAL_DENOISE_STATE *s)
{
    if (!s) return;
    free(s->history_q8);
    free(s->slow_history_q8);
    free(s->motion);
    free(s->spatial_row);
    memset(s, 0, sizeof(*s));
}

static uint32_t raw_difference(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint32_t raw_iir(uint32_t current, uint32_t previous, uint32_t shift)
{
    return (previous * ((1U << shift) - 1U) + current + (1U << (shift - 1U))) >> shift;
}

static int raw_reserve(RAW_TEMPORAL_DENOISE_STATE *s, size_t count,
    uint32_t width, int spatial, int dual)
{
    uint32_t *history = NULL, *slow = NULL;
    uint16_t *motion = NULL, *row = NULL;
    if (s->capacity < count) {
        history = (uint32_t *)malloc(count * sizeof(*history));
        if (!history) goto error;
    }
    if (dual && s->dual_capacity < count) {
        slow = (uint32_t *)malloc(count * sizeof(*slow));
        motion = (uint16_t *)malloc(count * sizeof(*motion));
        if (!slow || !motion) goto error;
    }
    if (spatial && s->row_capacity < width) {
        row = (uint16_t *)malloc((size_t)width * sizeof(*row));
        if (!row) goto error;
    }
    if (history) {
        free(s->history_q8);
        s->history_q8 = history;
        s->capacity = count;
    }
    if (slow) {
        free(s->slow_history_q8);
        free(s->motion);
        s->slow_history_q8 = slow;
        s->motion = motion;
        s->dual_capacity = count;
    }
    if (row) {
        free(s->spatial_row);
        s->spatial_row = row;
        s->row_capacity = width;
    }
    return 0;
error:
    free(history); free(slow); free(motion); free(row);
    return RAW_TEMPORAL_ERROR_MEMORY;
}

static uint32_t raw_dual_speed(uint32_t current, uint32_t original,
    uint32_t *fast, uint32_t *slow, uint16_t *motion,
    const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    uint32_t jump = raw_difference(current, *fast);
    uint32_t f = raw_iir(current, *fast, 1U);
    uint32_t s = raw_iir(current, *slow, 4U);
    uint32_t difference = raw_difference(f, s);
    uint32_t previous_mode = *motion >> 8, hold = *motion & 255U;
    uint32_t low = p->motion_threshold_dn * 256U;
    uint32_t high = p->motion_strong_threshold_dn * 256U;
    uint32_t hysteresis = p->motion_hysteresis_dn * 256U;
    uint32_t mode;
    if (previous_mode == 2U) high -= hysteresis;
    if (previous_mode != 0U) low -= hysteresis;
    mode = difference >= high || jump >= high ? 2U : difference >= low ? 1U : 0U;
    if (mode && mode >= previous_mode) {
        hold = p->motion_hold_frames;
    } else if (mode < previous_mode && hold) {
        mode = previous_mode;
        --hold;
    } else {
        hold = mode ? p->motion_hold_frames : 0U;
    }
    /* Reset slow history throughout motion, not only on the entry frame.
     * The input/previous-fast residual still detects steps after a reset. */
    if (mode == 2U) f = s = current;
    else if (mode == 1U) s = f;
    *fast = f;
    *slow = s;
    *motion = (uint16_t)((mode << 8) | hold);
    return mode == 2U ? original : mode == 1U ? f : s;
}

int raw_temporal_denoise_process(RAW_TEMPORAL_DENOISE_STATE *s,
    uint16_t *pixels, size_t stride, uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    uint32_t x, y, k;
    size_t count;
    int reset, spatial;
    if (!s || raw_temporal_denoise_validate(p)) return RAW_TEMPORAL_ERROR_PARAMS;
    if (!p->enable) { raw_temporal_denoise_reset(s); return 0; }
    if (!pixels || !width || !height || width > SIZE_MAX / height ||
        (uint64_t)width * sizeof(*pixels) > stride || stride % sizeof(*pixels) ||
        (height > 1U && stride > (SIZE_MAX - (size_t)width * 2U) / (height - 1U)))
        return RAW_TEMPORAL_ERROR_LAYOUT;
    count = (size_t)width * height;
    if (count > SIZE_MAX / sizeof(*s->history_q8)) return RAW_TEMPORAL_ERROR_LAYOUT;
    reset = !s->valid || width != s->width || height != s->height ||
        memcmp(&s->params, p, sizeof(*p)) || timestamp_us <= s->timestamp_us ||
        timestamp_us - s->timestamp_us > 250000U;
    spatial = p->spatial_enable && width > 2U && height > 2U;
    if (raw_reserve(s, count, width, spatial, p->dual_speed_enable))
        return RAW_TEMPORAL_ERROR_MEMORY;
    k = p->history_shift;
    for (y = 0; y < height; ++y) {
        uint16_t *row = (uint16_t *)((uint8_t *)pixels + y * stride);
        const uint16_t *south = y + 1U < height ?
            (const uint16_t *)((const uint8_t *)row + stride) : NULL;
        uint32_t *history = s->history_q8 + (size_t)y * width;
        uint32_t west = 0;
        for (x = 0; x < width; ++x) {
            uint32_t center = row[x], current = center << 8, value;
            if (spatial) {
                if (y && south && x && x + 1U < width) {
                    uint32_t north = s->spatial_row[x], east = row[x + 1U], down = south[x];
                    if (raw_difference(center, north) <= p->spatial_threshold_dn &&
                        raw_difference(center, down) <= p->spatial_threshold_dn &&
                        raw_difference(center, east) <= p->spatial_threshold_dn &&
                        raw_difference(center, west) <= p->spatial_threshold_dn)
                        current = (4U * center + north + down + east + west) << 5;
                }
                /* One row of original samples prevents in-place feedback. */
                s->spatial_row[x] = (uint16_t)center;
                west = center;
            }
            if (p->dual_speed_enable) {
                size_t index = (size_t)y * width + x;
                if (reset) {
                    history[x] = s->slow_history_q8[index] = current;
                    s->motion[index] = 0;
                    value = current;
                } else {
                    value = raw_dual_speed(current, center << 8, &history[x],
                        &s->slow_history_q8[index], &s->motion[index], p);
                }
            } else {
                uint32_t previous = reset ? current : history[x];
                value = raw_difference(current, previous) <= p->threshold_dn * 256U ?
                    raw_iir(current, previous, k) : current;
                history[x] = value;
            }
            row[x] = (uint16_t)((value + 128U) >> 8);
        }
    }
    s->width = width; s->height = height; s->timestamp_us = timestamp_us;
    s->params = *p; s->valid = 1;
    return reset ? 2 : 1;
}
