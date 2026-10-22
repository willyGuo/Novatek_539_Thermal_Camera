#include "agc_weighted.h"
#include "agc_mode.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

static AGC_WEIGHTED_PARAMS g_params = {20, 1, 100, 800, 0, 90};
static pthread_mutex_t g_params_lock = PTHREAD_MUTEX_INITIALIZER;

int agc_weighted_validate_params(const AGC_WEIGHTED_PARAMS *p)
{
    return !p || p->clip_tenths > 1000 || p->redistribute > 1 ||
        p->bright_percentile_tenths > 1000 || p->gain_limit_tenths > 40 ||
        p->temporal_previous_percent > 99 ||
        !(p->bright_weight_percent == 100 || p->bright_weight_percent == 105 ||
          (p->bright_weight_percent >= 110 && p->bright_weight_percent <= 200 &&
           p->bright_weight_percent % 10 == 0)) ? -1 : 0;
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
    if (ws) ws->temporal_valid = 0;
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
    uint64_t threshold, uint32_t bright)
{
    return ((uint64_t)h[i] + h[i + 1]) *
        ((uint64_t)i * 1000U + 500U >= threshold ? bright : 100U);
}

static double interval_weight_float(const uint32_t *h, uint32_t i,
    uint64_t threshold, double bright)
{
    double value = (double)((uint64_t)h[i] + h[i + 1]);
    return (uint64_t)i * 1000U + 500U >= threshold ? value * bright : value;
}

/* Pairwise reduction keeps the bright-weight float path consistent with the
 * Toolkit's NumPy sum; cumsum below remains sequential, as in the reference. */
static double weight_sum_float(const uint32_t *h, uint32_t first, uint32_t n,
    uint64_t threshold, double bright)
{
    uint32_t i;
    if (n < 8) {
        double sum = 0;
        for (i = 0; i < n; i++) sum += interval_weight_float(h, first + i, threshold, bright);
        return sum;
    }
    if (n <= 128) {
        double sums[8], sum;
        uint32_t j;
        for (j = 0; j < 8; j++) sums[j] = interval_weight_float(h, first + j, threshold, bright);
        for (i = 8; i < n - n % 8; i += 8)
            for (j = 0; j < 8; j++) sums[j] += interval_weight_float(h, first + i + j, threshold, bright);
        sum = ((sums[0] + sums[1]) + (sums[2] + sums[3])) +
              ((sums[4] + sums[5]) + (sums[6] + sums[7]));
        for (; i < n; i++) sum += interval_weight_float(h, first + i, threshold, bright);
        return sum;
    }
    i = (n / 2) & ~7U;
    return weight_sum_float(h, first, i, threshold, bright) +
        weight_sum_float(h, first + i, n - i, threshold, bright);
}

int agc_weighted_apply_u14(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws, const AGC_WEIGHTED_PARAMS *p)
{
    uint32_t x, y, i, first = 0, last = 0, unique = 0, lo, hi;
    uint32_t mapping_first, mapping_last, anchor;
    uint64_t pixels = (uint64_t)width * height, threshold, rank, total = 0, cumulative = 0;
    if (!input || !output || !ws || !width || !height || pixels > UINT32_MAX ||
        agc_weighted_validate_params(p) || input_stride < (size_t)width * 2 ||
        output_stride < (size_t)width * 2 || (input_stride | output_stride) & 1U ||
        height > SIZE_MAX / input_stride || height > SIZE_MAX / output_stride) return -1;
    if (ws->last_width != width || ws->last_height != height ||
        memcmp(&ws->last_params, p, sizeof(*p)) != 0) agc_weighted_state_reset(ws);
    memset(ws->histogram, 0, sizeof(ws->histogram));
    memset(ws->work, 0, sizeof(ws->work));
    memset(ws->lut, 0, sizeof(ws->lut));
    for (y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        for (x = 0; x < width; x++) ws->histogram[row[x] > 16383U ? 16383U : row[x]]++;
    }
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (ws->histogram[i]) {
        if (!unique) first = i;
        last = i; unique++;
    }
    anchor = order_dn(ws->histogram, (pixels - 1) / 2);
    if (unique <= 1) goto temporal; /* Unsmoothed constant mapping is zero. */
    rank = (pixels - 1) * p->bright_percentile_tenths;
    lo = order_dn(ws->histogram, rank / 1000);
    hi = order_dn(ws->histogram, rank / 1000 + ((rank % 1000) != 0));
    threshold = (uint64_t)lo * (1000 - rank % 1000) + (uint64_t)hi * (rank % 1000);
    memcpy(ws->work, ws->histogram, sizeof(ws->work));
    mapping_first = first; mapping_last = last;
    if (p->clip_tenths) {
        uint64_t cap64 = round_even(pixels * p->clip_tenths, (uint64_t)unique * 10U);
        uint32_t cap = cap64 > pixels ? (uint32_t)pixels : (uint32_t)cap64;
        uint64_t excess = 0, capacity = 0;
        if (!cap) cap = 1;
        for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
            if (ws->work[i] > cap) { excess += ws->work[i] - cap; ws->work[i] = cap; }
            capacity += cap - ws->work[i];
        }
        if (p->redistribute) {
            mapping_first = 0; mapping_last = 16383;
            if (excess > capacity) excess = capacity;
            while (excess) {
                uint32_t available = 0, at = 0;
                uint64_t batch, remainder, k = 0, added = 0;
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) available += ws->work[i] < cap;
                if (!available) break;
                batch = excess / available; remainder = excess % available;
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (ws->work[i] < cap) {
                    uint64_t inc = batch, room = cap - ws->work[i];
                    if (inc > room) inc = room;
                    if (k < remainder && at == k * available / remainder) {
                        if (inc < room) inc++;
                        k++;
                    }
                    ws->work[i] += (uint32_t)inc; added += inc; at++;
                }
                if (!added) return -1;
                excess -= added;
            }
        }
    }
    if (p->bright_weight_percent == 100) {
        for (i = mapping_first; i < mapping_last; i++)
            total += interval_weight(ws->work, i, threshold, 100);
        if (!total) goto temporal;
        for (i = mapping_first; i < mapping_last; i++) {
            cumulative += interval_weight(ws->work, i, threshold, 100);
            ws->lut[i + 1] = (uint8_t)round_even(cumulative * 255U, total);
        }
    } else {
        double bright = p->bright_weight_percent / 100.0, sum = 0;
        double total_float = weight_sum_float(ws->work, mapping_first,
            mapping_last - mapping_first, threshold, bright);
        if (total_float <= 0) goto temporal;
        for (i = mapping_first; i < mapping_last; i++) {
            double mapped, fraction;
            uint32_t q;
            sum += interval_weight_float(ws->work, i, threshold, bright);
            mapped = sum * 255.0 / total_float;
            q = (uint32_t)mapped;
            fraction = mapped - q;
            q += fraction > 0.5 || (fraction == 0.5 && (q & 1U));
            ws->lut[i + 1] = (uint8_t)(q > 255 ? 255 : q);
        }
    }
    for (i = mapping_last + 1; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = 255;
temporal:
    /* Match Fieldscale's current/previous blend and round-half-up. Keep 0.1
     * Gray precision to avoid whole-Gray temporal sticking. Apply Gain Limit
     * afterward so smoothing/rounding cannot violate its fractional slope cap. */
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
        uint32_t value = ws->lut[i] * 10U;
        if (ws->temporal_valid && p->temporal_previous_percent)
            value = (value * (100U - p->temporal_previous_percent) +
                ws->previous_lut_tenths[i] * p->temporal_previous_percent + 50U) / 100U;
        ws->previous_lut_tenths[i] = (uint16_t)value;
        ws->work[i] = value;
    }
    ws->last_params = *p;
    ws->last_width = width;
    ws->last_height = height;
    ws->temporal_valid = 1;
    if (p->gain_limit_tenths) {
        int32_t shift;
        uint32_t previous = ws->work[0];
        ws->work[0] = 0;
        for (i = 1; i < AGC_WEIGHTED_BINS; i++) {
            uint32_t current = ws->work[i];
            uint32_t step = current - previous;
            previous = current;
            if (step > p->gain_limit_tenths) step = p->gain_limit_tenths;
            ws->work[i] = ws->work[i - 1] + step;
        }
        shift = (int32_t)ws->previous_lut_tenths[anchor] - (int32_t)ws->work[anchor];
        for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
            int32_t v = (int32_t)ws->work[i] + shift;
            if (v < 0) v = 0;
            if (v > 2550) v = 2550;
            ws->work[i] = (uint32_t)v;
        }
    }
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = (uint8_t)((ws->work[i] + 5) / 10);
    for (y = 0; y < height; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *out = (uint16_t *)((uint8_t *)output + y * output_stride);
        for (x = 0; x < width; x++) {
            uint32_t dn = in[x] > 16383U ? 16383U : in[x];
            out[x] = (uint16_t)((ws->lut[dn] * 16383U + 127U) / 255U);
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
    printf("Clip=%.1f, Redistribution=%u, Bright Weight=%.2f, P=%.1f, Gain Limit=%.1f Gray/RAW DN\n",
        p.clip_tenths / 10.0, p.redistribute, p.bright_weight_percent / 100.0,
        p.bright_percentile_tenths / 10.0, p.gain_limit_tenths / 10.0);
    printf("Gain Limit: 0 off; preserve lower-median brightness; no rescaling.\n");
    printf("LUT temporal smoothing: previous %u%% / current %u%% (0 previous: off)\n",
        p.temporal_previous_percent, 100U - p.temporal_previous_percent);
    printf("Settings: CFG Setting -> AGC setting -> Weighted Mapping\n0. Back\n>> ");
    (void)fgets(buf, sizeof(buf), stdin);
    return 0;
}
