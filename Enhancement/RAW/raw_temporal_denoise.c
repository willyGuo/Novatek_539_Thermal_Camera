#include "raw_denoise_internal.h"
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
        p->spatial_enable > 1U || p->spatial_threshold_dn > 16383U || p->spatial_mode > 1U ||
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

int raw_temporal_denoise_process_to(RAW_TEMPORAL_DENOISE_STATE *s,
    const uint16_t *input, size_t input_stride, uint16_t *output, size_t output_stride,
    uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    RAW_KERNEL_PARAMS kernel;
    unsigned int path;
    size_t count, input_size, output_size;
    uintptr_t input_address = (uintptr_t)input, output_address = (uintptr_t)output;
    int reset, spatial;
    if (!s || raw_temporal_denoise_validate(p)) return RAW_TEMPORAL_ERROR_PARAMS;
    if (!p->enable) { raw_temporal_denoise_reset(s); return 0; }
    if (!input || !output || (input_address | output_address) % sizeof(uint16_t) ||
        !width || !height || width > SIZE_MAX / height ||
        (uint64_t)width * sizeof(*input) > input_stride || input_stride % sizeof(*input) ||
        (uint64_t)width * sizeof(*output) > output_stride || output_stride % sizeof(*output) ||
        (height > 1U && (input_stride > (SIZE_MAX - (size_t)width * 2U) / (height - 1U) ||
            output_stride > (SIZE_MAX - (size_t)width * 2U) / (height - 1U))))
        return RAW_TEMPORAL_ERROR_LAYOUT;
    input_size = (height - 1U) * input_stride + (size_t)width * 2U;
    output_size = (height - 1U) * output_stride + (size_t)width * 2U;
    if (input_address > UINTPTR_MAX - input_size || output_address > UINTPTR_MAX - output_size ||
        ((input_address != output_address || input_stride != output_stride) &&
         input_address < output_address + output_size && output_address < input_address + input_size))
        return RAW_TEMPORAL_ERROR_LAYOUT;
    count = (size_t)width * height;
    if (count > SIZE_MAX / sizeof(*s->history_q8)) return RAW_TEMPORAL_ERROR_LAYOUT;
    reset = !s->valid || width != s->width || height != s->height ||
        memcmp(&s->params, p, sizeof(*p)) || timestamp_us <= s->timestamp_us ||
        timestamp_us - s->timestamp_us > 250000U;
    spatial = p->spatial_enable && width > 2U && height > 2U;
    if (raw_reserve(s, count, width, spatial, p->dual_speed_enable))
        return RAW_TEMPORAL_ERROR_MEMORY;
    kernel.threshold = p->threshold_dn << 8;
    kernel.shift = p->history_shift;
    kernel.weight = (1U << p->history_shift) - 1U;
    kernel.rounding = 1U << (p->history_shift - 1U);
    kernel.spatial_threshold = p->spatial_threshold_dn;
    kernel.low = p->motion_threshold_dn << 8;
    kernel.high = p->motion_strong_threshold_dn << 8;
    kernel.hysteresis = p->motion_hysteresis_dn << 8;
    kernel.hold = p->motion_hold_frames;
    /* Spatial: 0 off, 1 strict, 2 selective; bit 2 dual, bit 3 reset. */
    path = (spatial ? p->spatial_mode + 1U : 0U) |
        (p->dual_speed_enable ? 4U : 0U) | (reset ? 8U : 0U);
    switch (path) {
    case 0: raw_legacy(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 1: raw_spatial_legacy(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 2: raw_selective_legacy(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 4: raw_dual(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 5: raw_spatial_dual(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 6: raw_selective_dual_separate(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 8: raw_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 9: raw_spatial_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 10: raw_selective_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 12: raw_dual_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 13: raw_spatial_dual_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    case 14: raw_selective_dual_init(s, input, input_stride, output, output_stride, width, height, &kernel); break;
    }
    s->width = width; s->height = height; s->timestamp_us = timestamp_us;
    s->params = *p; s->valid = 1;
    return reset ? 2 : 1;
}

/* Keep the established in-place API and its disabled/reset behavior. */
int raw_temporal_denoise_process(RAW_TEMPORAL_DENOISE_STATE *s,
    uint16_t *pixels, size_t stride, uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    return raw_temporal_denoise_process_to(s, pixels, stride, pixels, stride,
        width, height, timestamp_us, p);
}
