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
        !p->history_shift || p->history_shift > 3U ? -1 : 0;
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
    memset(s, 0, sizeof(*s));
}
int raw_temporal_denoise_process(RAW_TEMPORAL_DENOISE_STATE *s,
    uint16_t *pixels, size_t stride, uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p)
{
    uint32_t x, y, k;
    size_t count;
    int reset;
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
    if (s->capacity < count) {
        uint32_t *new_history = (uint32_t *)malloc(count * sizeof(*new_history));
        if (!new_history) return RAW_TEMPORAL_ERROR_MEMORY;
        free(s->history_q8);
        s->history_q8 = new_history;
        s->capacity = count;
        reset = 1;
    }
    k = p->history_shift;
    for (y = 0; y < height; ++y) {
        uint16_t *row = (uint16_t *)((uint8_t *)pixels + y * stride);
        uint32_t *history = s->history_q8 + (size_t)y * width;
        for (x = 0; x < width; ++x) {
            uint32_t current = (uint32_t)row[x] << 8;
            uint32_t previous = reset ? current : history[x];
            uint32_t difference = current > previous ? current - previous : previous - current;
            uint32_t value = difference <= p->threshold_dn * 256U ?
                (previous * ((1U << k) - 1U) + current + (1U << (k - 1U))) >> k : current;
            history[x] = value;
            row[x] = (uint16_t)((value + 128U) >> 8);
        }
    }
    s->width = width; s->height = height; s->timestamp_us = timestamp_us;
    s->params = *p; s->valid = 1;
    return reset ? 2 : 1;
}
