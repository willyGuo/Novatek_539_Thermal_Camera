#include "agc_weighted.h"
#include "agc_mode.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>

static AGC_WEIGHTED_PARAMS g_params = AGC_WEIGHTED_DEFAULT_PARAMS;
void agc_weighted_default_params(AGC_WEIGHTED_PARAMS *p)
{
    static const AGC_WEIGHTED_PARAMS defaults = AGC_WEIGHTED_DEFAULT_PARAMS;
    if (p) memcpy(p, &defaults, sizeof(*p));
}
static pthread_mutex_t g_params_lock = PTHREAD_MUTEX_INITIALIZER;

int agc_weighted_validate_params(const AGC_WEIGHTED_PARAMS *p)
{
    return !p || p->clip_tenths > 1000 || p->redistribute > 1 ||
        p->bright_percentile_tenths > 1000 || p->gain_limit_tenths > 40 ||
        p->temporal_previous_percent > 99 ||
        (!isfinite(p->bright_weight) || p->bright_weight < 1.0) ? -1 : 0;
}
void agc_weighted_get_params(AGC_WEIGHTED_PARAMS *p)
{
    if (!p) return;
    pthread_mutex_lock(&g_params_lock); *p = g_params; pthread_mutex_unlock(&g_params_lock);
}
int agc_weighted_set_params(const AGC_WEIGHTED_PARAMS *p)
{
    if (agc_weighted_validate_params(p)) return -1;
    pthread_mutex_lock(&g_params_lock); g_params = *p; pthread_mutex_unlock(&g_params_lock);
    return 0;
}
int agc_weighted_is_enabled(void) { return agc_mode_get() == AGC_MODE_WEIGHTED_CPU; }
void agc_weighted_state_reset(AGC_WEIGHTED_WORKSPACE *ws)
{
    if (ws) {
        ws->temporal_valid = 0;
        ws->gain_offset_valid = 0;
    }
}

static uint64_t round_even(uint64_t n, uint64_t d)
{
    uint64_t q = n / d, r = n % d;
    return q + (r > d - r || (r == d - r && (q & 1U)));
}
static uint32_t order_dn(const uint32_t *h, uint64_t index)
{
    uint32_t i; uint64_t sum = 0;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
        sum += h[i];
        if (sum > index) return i;
    }
    return AGC_WEIGHTED_BINS - 1;
}
static uint64_t interval_weight(const uint32_t *h, uint32_t i,
    uint32_t threshold, uint32_t bright)
{
    return ((uint64_t)h[i] + h[i + 1]) *
        (i >= threshold ? bright : 100U);
}

static double interval_weight_float(const uint32_t *h, uint32_t i,
    uint32_t threshold, double bright, int scaled)
{
    double value = (double)((uint64_t)h[i] + h[i + 1]);
    if (scaled)
        return i >= threshold ? value : value / bright;
    return i >= threshold ? value * bright : value;
}

/* Pairwise reduction keeps the bright-weight float path consistent with the
 * Toolkit's NumPy sum; cumsum below remains sequential, as in the reference. */
static double weight_sum_float(const uint32_t *h, uint32_t first, uint32_t n,
    uint32_t threshold, double bright, int scaled)
{
    uint32_t i;
    if (n < 8) {
        double sum = 0;
        for (i = 0; i < n; i++) sum += interval_weight_float(h, first + i, threshold, bright, scaled);
        return sum;
    }
    if (n <= 128) {
        double sums[8], sum;
        uint32_t j;
        for (j = 0; j < 8; j++) sums[j] = interval_weight_float(h, first + j, threshold, bright, scaled);
        for (i = 8; i < n - n % 8; i += 8)
            for (j = 0; j < 8; j++) sums[j] += interval_weight_float(h, first + i + j, threshold, bright, scaled);
        sum = ((sums[0] + sums[1]) + (sums[2] + sums[3])) +
              ((sums[4] + sums[5]) + (sums[6] + sums[7]));
        for (; i < n; i++) sum += interval_weight_float(h, first + i, threshold, bright, scaled);
        return sum;
    }
    i = (n / 2) & ~7U;
    return weight_sum_float(h, first, i, threshold, bright, scaled) +
        weight_sum_float(h, first + i, n - i, threshold, bright, scaled);
}

static int output_weighted(const uint16_t *, size_t, uint16_t *, size_t,
    uint32_t, uint32_t, AGC_WEIGHTED_WORKSPACE *);
int agc_weighted_apply_u14(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws, const AGC_WEIGHTED_PARAMS *p)
{
    uint32_t x, y, i, first = 0, last = 0, unique = 0;
    uint32_t mapping_first, mapping_last, threshold;
    uint64_t pixels = (uint64_t)width * height, rank, total = 0, cumulative = 0;
    if (!input || !output || !ws || !width || !height || pixels > UINT32_MAX ||
        agc_weighted_validate_params(p) || input_stride < (size_t)width * 2 ||
        output_stride < (size_t)width * 2 || (input_stride | output_stride) & 1U ||
        height > SIZE_MAX / input_stride || height > SIZE_MAX / output_stride) return -1;
    if (ws->last_width != width || ws->last_height != height ||
        ws->last_params.clip_tenths != p->clip_tenths ||
        ws->last_params.redistribute != p->redistribute ||
        ws->last_params.bright_weight != p->bright_weight ||
        ws->last_params.bright_percentile_tenths != p->bright_percentile_tenths ||
        ws->last_params.gain_limit_tenths != p->gain_limit_tenths ||
        ws->last_params.temporal_previous_percent != p->temporal_previous_percent)
        agc_weighted_state_reset(ws);
    memset(ws->histogram, 0, sizeof(ws->histogram));
    memset(ws->scratch, 0, sizeof(ws->scratch));
    memset(ws->lut, 0, sizeof(ws->lut));
    for (y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        for (x = 0; x < width; x++) ws->histogram[row[x] > 16383U ? 16383U : row[x]]++;
    }

    for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (ws->histogram[i]) {
        if (!unique) first = i;
        last = i; unique++;
    }
    if (unique <= 1) goto temporal; /* Unsmoothed constant mapping is zero. */
    memcpy(ws->scratch, ws->histogram, sizeof(ws->scratch));
    mapping_first = first; mapping_last = last;
    if (p->clip_tenths) {
        uint64_t cap64 = round_even(pixels * p->clip_tenths, (uint64_t)unique * 10U);
        uint32_t cap = cap64 > pixels ? (uint32_t)pixels : (uint32_t)cap64;
        uint64_t excess = 0, capacity = 0;
        if (!cap) cap = 1;
        for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
            if (ws->scratch[i] > cap) { excess += ws->scratch[i] - cap; ws->scratch[i] = cap; }
            if (ws->histogram[i]) capacity += cap - ws->scratch[i];
        }
        if (p->redistribute) {
            if (excess > capacity) excess = capacity;
            while (excess) {
                uint32_t available = 0, at = 0;
                uint64_t batch, remainder, k = 0, added = 0;
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) available += ws->histogram[i] != 0 && ws->scratch[i] < cap;
                if (!available) break;
                batch = excess / available; remainder = excess % available;
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (ws->histogram[i] && ws->scratch[i] < cap) {
                    uint64_t inc = batch, room = cap - ws->scratch[i];
                    if (inc > room) inc = room;
                    if (k < remainder && at == k * available / remainder) {
                        if (inc < room) inc++;
                        k++;
                    }
                    ws->scratch[i] += (uint32_t)inc; added += inc; at++;
                }
                if (!added) return -1;
                excess -= added;
            }
        }
    }
    /* Percentile uses the retained counts, including any discarded excess. */
    {
        uint64_t processed_pixels = 0;
        for (i = first; i <= last; i++) processed_pixels += ws->scratch[i];
        if (!processed_pixels) goto temporal;
        /* Integer inverse CDF: first DN with cumulative count / N > P.
         * At P100, use the last retained DN since CDF cannot exceed 1.
         * Integer k and threshold make k + 0.5 >= threshold equal to k >= threshold.
         */
        rank = processed_pixels * p->bright_percentile_tenths / 1000U;
        if (rank >= processed_pixels) rank = processed_pixels - 1;
        threshold = order_dn(ws->scratch, rank);
    }
    if (p->bright_weight == 1.0) {
        for (i = mapping_first; i < mapping_last; i++)
            total += interval_weight(ws->scratch, i, threshold, 100);
        if (!total) goto temporal;
        for (i = mapping_first; i < mapping_last; i++) {
            cumulative += interval_weight(ws->scratch, i, threshold, 100);
            ws->lut[i + 1] = (uint16_t)round_even(cumulative * 16383U, total);
        }
    } else {
        double bright = p->bright_weight, sum = 0, base_weight = 0;
        int scaled;
        for (i = mapping_first; i < mapping_last; i++)
            base_weight += (double)ws->scratch[i] + ws->scratch[i + 1];
        /* Common scaling matches Toolkit and keeps finite multipliers safe. */
        scaled = bright > DBL_MAX / fmax(1.0, 512.0 * base_weight);
        double total_float = weight_sum_float(ws->scratch, mapping_first,
            mapping_last - mapping_first, threshold, bright, scaled);
        if (total_float <= 0) goto temporal;
        for (i = mapping_first; i < mapping_last; i++) {
            double mapped, fraction;
            uint32_t q;
            sum += interval_weight_float(ws->scratch, i, threshold, bright, scaled);
            mapped = sum * 16383.0 / total_float;
            q = (uint32_t)mapped;
            fraction = mapped - q;
            q += fraction > 0.5 || (fraction == 0.5 && (q & 1U));
            ws->lut[i + 1] = (uint16_t)(q > 16383 ? 16383 : q);
        }
    }
    for (i = mapping_last + 1; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = 16383;
temporal:
    /* Keep native RAW14 DN in Q8 during LUT blending and slope limiting. Apply Gain Limit
     * afterward so rounding cannot violate its fractional cap.
     */
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
        uint32_t value = ws->lut[i] * 256U;
        if (ws->temporal_valid && p->temporal_previous_percent)
            value = (value * (100U - p->temporal_previous_percent) +
                ws->previous_lut_q8[i] * p->temporal_previous_percent + 50U) / 100U;
        ws->previous_lut_q8[i] = value;
        ws->scratch[i] = value;
    }
    ws->last_params = *p;
    ws->last_width = width;
    ws->last_height = height;
    ws->temporal_valid = 1;
    if (p->gain_limit_tenths) {
        uint64_t difference_sum = 0;
        uint32_t offset_q8;
        uint32_t limit_q8 = (uint32_t)round_even(
            (uint64_t)p->gain_limit_tenths * 16383U * 256U, 2550U);
        uint32_t previous = ws->scratch[0];
        ws->scratch[0] = 0;
        for (i = 1; i < AGC_WEIGHTED_BINS; i++) {
            uint32_t current = ws->scratch[i];
            /* Monotone LUT: delta >= 0, so min(delta, limit) needs no max(delta, 0). */
            uint32_t step = current - previous;
            previous = current;
            if (step > limit_q8) step = limit_q8;
            ws->scratch[i] = ws->scratch[i - 1] + step;
        }
        /* A one-DN median change can jump across an almost 255-Gray LUT
         * step in a low-contrast scene. Preserve the histogram-weighted
         * mean instead: count changes contribute continuously, without a
         * discrete DN anchor. work <= the original monotone LUT here. */
        for (i = 0; i < AGC_WEIGHTED_BINS; i++)
            difference_sum += (uint64_t)ws->histogram[i] *
                (ws->previous_lut_q8[i] - ws->scratch[i]);
        offset_q8 = (uint32_t)((difference_sum + pixels / 2U) / pixels);
        if (ws->gain_offset_valid && p->temporal_previous_percent)
            offset_q8 = (offset_q8 * (100U - p->temporal_previous_percent) +
                ws->gain_offset_q8 * p->temporal_previous_percent + 50U) / 100U;
        ws->gain_offset_q8 = offset_q8;
        ws->gain_offset_valid = 1;
        for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
            int32_t v = (int32_t)ws->scratch[i] + (int32_t)offset_q8;
            if (v < 0) v = 0;
            if (v > 16383 * 256) v = 16383 * 256;
            ws->scratch[i] = (uint32_t)v;
        }
    } else {
        ws->gain_offset_valid = 0;
    }
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = (uint16_t)((ws->scratch[i] + 128U) / 256U);
    return output_weighted(input, input_stride, output, output_stride, width, height, ws);
}

static int output_weighted(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws)
{
    uint32_t x, y;
    for (y = 0; y < height; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *out = (uint16_t *)((uint8_t *)output + y * output_stride);
        for (x = 0; x < width; x++) {
            uint32_t dn = in[x] > 16383U ? 16383U : in[x];
            out[x] = ws->lut[dn];
        }
    }
    return 0;
}

int agc_weighted_menu_run(void)
{
    char buf[32];
    AGC_WEIGHTED_PARAMS p;
    agc_weighted_get_params(&p);
    printf("Weighted Mapping AGC (CPU), global 1x1: %s\n", agc_weighted_is_enabled() ? "on" : "off");
    printf("Clip=%.1f, Redistribution=%u, Bright Weight=%.17g, P=%.1f, Gain Limit=%.1f Gray/RAW DN\n",
        p.clip_tenths / 10.0, p.redistribute, p.bright_weight,
        p.bright_percentile_tenths / 10.0, p.gain_limit_tenths / 10.0);
    printf("Redistribution: originally populated DN bins only; retain Clip cap; discard excess without capacity.\n");
    printf("Mapping: native RAW14 LUT/output; no intermediate Gray8 quantization.\n");
    printf("Gain Limit: 0 off; mean-brightness compensation with temporal smoothing; no rescaling.\n");
    printf("LUT / Gain brightness smoothing: previous %u%% / current %u%% (0 previous: off)\n",
        p.temporal_previous_percent, 100U - p.temporal_previous_percent);
    printf("Settings: CFG Setting -> AGC setting -> Weighted Mapping\n0. Back\n>> ");
    (void)fgets(buf, sizeof(buf), stdin);
    return 0;
}
