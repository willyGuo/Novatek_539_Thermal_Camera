#include "agc_weighted.h"
#include "agc_mode.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>

static AGC_WEIGHTED_PARAMS g_params = AGC_WEIGHTED_DEFAULT_PARAMS;
enum { BALANCED_REACQUIRE_MIN_FRAMES = 8, BALANCED_REACQUIRE_MAX_FRAMES = 30 };
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
        p->temporal_previous_percent > 99 || p->balanced_background > 1 ||
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
        ws->balanced_background_valid = 0;
        ws->balanced_candidate_frames = 0;
        ws->reacquire_remaining = 0;
        ws->reacquire_stable_frames = 0;
    }
}

void agc_weighted_begin_reacquire(AGC_WEIGHTED_WORKSPACE *ws)
{
    agc_weighted_state_reset(ws);
    if (ws) ws->reacquire_remaining = BALANCED_REACQUIRE_MAX_FRAMES;
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

/* Linear percentile of integer counts; histogram-only, no sample expansion. */
static double balanced_percentile(const uint32_t *h, uint64_t pixels,
    uint32_t numerator, uint32_t denominator)
{
    uint64_t rank = (pixels - 1U) * numerator, index = rank / denominator;
    uint32_t rem = (uint32_t)(rank % denominator), low = order_dn(h, index);
    uint32_t high = order_dn(h, index + (rem != 0));
    return low + (high - low) * (rem / (double)denominator);
}

/* Prefer a supported low-DN group even when hot pixels are the majority.
 * Match Toolkit v2: >=0.3% and >=2 samples on each side, mean separation
 * >=70 DN; prefer the first >=12-empty-bin gap, otherwise Otsu >=85% variance.
 * No confident split: keep global median and let the shadow toe protect it.
 */
static double balanced_background_center(const uint32_t *h, uint64_t pixels, int *confident)
{
    uint32_t i, next;
    uint64_t weighted = 0, left = 0, left_weighted = 0, best_pixels = 0;
    uint64_t minimum = (pixels * 3U + 999U) / 1000U;
    double mean, variance = 0, best_score = -1;
    *confident = 0;
    if (minimum < 2) minimum = 2;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) weighted += (uint64_t)i * h[i];
    mean = weighted / (double)pixels;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (h[i]) {
        double difference = i - mean;
        variance += h[i] * (difference * difference);
    }
    variance /= pixels;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (h[i]) {
        uint64_t right;
        double delta, score;
        left += h[i]; left_weighted += (uint64_t)i * h[i];
        right = pixels - left;
        if (left < minimum || right < minimum) continue;
        delta = (weighted - left_weighted) / (double)right - left_weighted / (double)left;
        if (delta < 70.0) continue;
        next = i + 1U;
        while (next < AGC_WEIGHTED_BINS && !h[next]) next++;
        if (next < AGC_WEIGHTED_BINS && next - i - 1U >= 12U) {
            *confident = 1;
            return balanced_percentile(h, left, 1, 2);
        }
        score = (double)left * right * (delta * delta);
        if (variance > 0 && score / ((double)pixels * pixels * variance) >= 0.85 && score > best_score) {
            best_score = score; best_pixels = left;
        }
    }
    *confident = best_pixels != 0;
    return balanced_percentile(h, best_pixels ? best_pixels : pixels, 1, 2);
}

static uint64_t balanced_hot_count(const uint32_t *h, double center)
{
    uint64_t count = 0;
    uint32_t i = (uint32_t)fmin(AGC_WEIGHTED_BINS, floor(center + 40.0) + 1.0);
    for (; i < AGC_WEIGHTED_BINS; i++) count += h[i];
    return count;
}

static uint32_t balanced_follow_q8(uint32_t previous, uint32_t desired,
    uint32_t previous_percent, uint32_t maximum_step)
{
    uint32_t difference = previous > desired ? previous - desired : desired - previous;
    uint32_t step = (uint32_t)round_even((uint64_t)difference * (100U - previous_percent), 100U);
    if (step > maximum_step) step = maximum_step;
    return desired >= previous ? previous + step : previous - step;
}

/* A changing hot-area ratio must not replace a reliable cool center with a
 * warm global median in one frame. Only background parameters are tracked;
 * pixels are never temporally averaged here. Smooth=0 remains stateless. */
static double balanced_track_center(AGC_WEIGHTED_WORKSPACE *ws,
    const AGC_WEIGHTED_PARAMS *p, uint64_t pixels, double candidate,
    int confident, double high)
{
    uint32_t desired = (uint32_t)floor(candidate * 256.0 + 0.5);
    if (!ws->balanced_background_valid || !p->temporal_previous_percent) {
        ws->balanced_center_q8 = desired;
        ws->balanced_candidate_q8 = desired;
        ws->balanced_candidate_frames = 0;
    } else {
        uint32_t current = ws->balanced_center_q8;
        uint32_t difference = current > desired ? current - desired : desired - current;
        double previous = current / 256.0;
        if (!confident && high >= previous + 70.0 &&
            balanced_hot_count(ws->histogram, previous) * 1000U >= pixels * 3U) {
            /* Hot fallback: keep the reliable anchor until a cool group returns. */
            ws->balanced_candidate_frames = 0;
            return previous;
        }
        if (difference > 4U * 256U) {
            uint32_t pending = ws->balanced_candidate_q8;
            uint32_t distance = pending > desired ? pending - desired : desired - pending;
            if (!ws->balanced_candidate_frames || distance > 4U * 256U)
                ws->balanced_candidate_frames = 1;
            else if (ws->balanced_candidate_frames < 3U)
                ws->balanced_candidate_frames++;
            ws->balanced_candidate_q8 = desired;
            if (ws->balanced_candidate_frames < 3U) return previous;
        } else {
            ws->balanced_candidate_frames = 0;
        }
        ws->balanced_center_q8 = balanced_follow_q8(current, desired,
            p->temporal_previous_percent, 256U); /* At most1 input DN/frame. */
    }
    return ws->balanced_center_q8 / 256.0;
}

/* Toolkit balanced_background_v2 in native RAW14 units. Reuses existing
 * histogram/scratch/LUT arrays, preserves original histogram and temporal state.
 * It deliberately avoids the Toolkit's intermediate Gray8 LUT quantization.
 */
static int balanced_build_lut(AGC_WEIGHTED_WORKSPACE *ws,
    const AGC_WEIGHTED_PARAMS *p, uint64_t pixels)
{
    uint32_t i, first = 0, last = 0, unique = 0, knee;
    uint64_t detected = 0, hot_pixels = 0, retained = 0;
    int confident;
    int tracking;
    double center = balanced_background_center(ws->histogram, pixels, &confident);
    double high = balanced_percentile(ws->histogram, pixels, 9995, 10000);
    double gain = p->gain_limit_tenths ? fmin(1.2, p->gain_limit_tenths / 10.0) : 1.2;
    double target = 112.0, activation = 0, low_value = 0, upper_value = 0;
    int active, hot_curve = 0;
    double shadow_span, shadow_value, shadow_step;
    if (ws->reacquire_remaining && p->temporal_previous_percent) {
        uint32_t candidate = (uint32_t)floor(center * 256.0 + 0.5);
        uint32_t previous = ws->reacquire_center_q8;
        uint32_t distance = previous > candidate ? previous - candidate : candidate - previous;
        double low = balanced_percentile(ws->histogram, pixels, 5, 10000);
        /* A uniform/transition frame may be displayed, but must not lock in
         * the slow tracker. Rebuild both anchors and LUT from each fresh frame.
         * Require >=8 frames and 3 stable, nonuniform candidates; bound this
         * phase to 30 processed frames so a truly uniform scene also completes. */
        if (high - low >= 4.0 && distance <= 4U * 256U)
            ws->reacquire_stable_frames++;
        else
            ws->reacquire_stable_frames = high - low >= 4.0 ? 1U : 0U;
        ws->reacquire_center_q8 = candidate;
        ws->reacquire_remaining--;
        if (ws->reacquire_remaining <= BALANCED_REACQUIRE_MAX_FRAMES - BALANCED_REACQUIRE_MIN_FRAMES &&
            ws->reacquire_stable_frames >= 3U)
            ws->reacquire_remaining = 0;
        ws->balanced_background_valid = 0;
        ws->temporal_valid = 0;
        ws->gain_offset_valid = 0;
    } else {
        ws->reacquire_remaining = 0;
    }
    tracking = ws->balanced_background_valid && p->temporal_previous_percent;
    center = balanced_track_center(ws, p, pixels, center, confident, high);
    knee = (uint32_t)fmin(16383.0, ceil(center + 20.0));
    detected = balanced_hot_count(ws->histogram, center);
    active = detected * 1000U >= pixels * 3U && high >= center + 70.0;
    if (active) activation = fmin(1.0, detected / (double)pixels / 0.015);
    {
        uint32_t desired = (uint32_t)floor((112.0 - 20.0 * activation) * 256.0 + 0.5);
        ws->balanced_target_q8 = tracking ? balanced_follow_q8(ws->balanced_target_q8,
            desired, p->temporal_previous_percent, 128U) : desired;
        target = ws->balanced_target_q8 / 256.0;
        ws->balanced_background_valid = 1;
    }
    if (active) {
        memset(ws->scratch, 0, sizeof(ws->scratch));
        for (i = knee + 1U; i < AGC_WEIGHTED_BINS; i++) if (ws->histogram[i]) {
            if (!unique) first = i;
            last = i; unique++;
            hot_pixels += ws->histogram[i];
            ws->scratch[i] = ws->histogram[i];
        }
        if (unique > 1) {
            uint64_t threshold;
            double total = 0, cumulative = 0, base = 0, bright = p->bright_weight;
            int scaled;
            if (p->clip_tenths) {
                uint64_t cap64 = round_even(hot_pixels * p->clip_tenths, (uint64_t)unique * 10U);
                uint32_t cap = (uint32_t)(cap64 > hot_pixels ? hot_pixels : cap64);
                uint64_t excess = 0, capacity = 0;
                if (!cap) cap = 1;
                for (i = first; i <= last; i++) {
                    if (ws->scratch[i] > cap) { excess += ws->scratch[i] - cap; ws->scratch[i] = cap; }
                    if (ws->histogram[i]) capacity += cap - ws->scratch[i];
                }
                if (p->redistribute) {
                    if (excess > capacity) excess = capacity;
                    while (excess) {
                        uint32_t available = 0, at = 0;
                        uint64_t batch, remainder, k = 0, added = 0;
                        for (i = first; i <= last; i++) available += ws->histogram[i] != 0 && ws->scratch[i] < cap;
                        if (!available) break;
                        batch = excess / available; remainder = excess % available;
                        for (i = first; i <= last; i++) if (ws->histogram[i] && ws->scratch[i] < cap) {
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
            for (i = first; i <= last; i++) retained += ws->scratch[i];
            if (!retained) return -1;
            /* Only this opt-in path matches Toolkit's interpolated percentile.
             * Classic keeps the separately specified integer inverse CDF. */
            {
                uint64_t rank = (retained - 1U) * p->bright_percentile_tenths;
                uint32_t rem = (uint32_t)(rank % 1000U);
                uint32_t lo = order_dn(ws->scratch, rank / 1000U);
                uint32_t hi = order_dn(ws->scratch, rank / 1000U + (rem != 0));
                threshold = (uint64_t)lo * (1000U - rem) + (uint64_t)hi * rem;
            }
            for (i = first; i < last; i++) base += (double)ws->scratch[i] + ws->scratch[i + 1];
            scaled = bright > DBL_MAX / fmax(1.0, 512.0 * base);
            for (i = first; i < last; i++) {
                double weight = (double)ws->scratch[i] + ws->scratch[i + 1];
                int is_bright = (uint64_t)i * 1000U + 500U >= threshold;
                weight = scaled ? (is_bright ? weight : weight / bright) : (is_bright ? weight * bright : weight);
                total += weight;
            }
            if (!(total > 0)) return -1;
            for (i = first; i < last; i++) {
                double weight = (double)ws->scratch[i] + ws->scratch[i + 1];
                int is_bright = (uint64_t)i * 1000U + 500U >= threshold;
                weight = scaled ? (is_bright ? weight : weight / bright) : (is_bright ? weight * bright : weight);
                cumulative += weight;
                ws->lut[i + 1] = (uint16_t)fmin(16383.0, floor(cumulative / total * 16383.0 + 0.5));
            }
            for (i = last + 1U; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = 16383;
            if (p->gain_limit_tenths) {
                uint32_t cap = (uint32_t)round_even((uint64_t)p->gain_limit_tenths * 16383U * 256U, 2550U);
                uint32_t median = order_dn(ws->histogram, pixels - hot_pixels + (hot_pixels - 1U) / 2U);
                int64_t offset;
                ws->scratch[0] = 0;
                for (i = 1; i < AGC_WEIGHTED_BINS; i++) {
                    uint32_t step = (ws->lut[i] - ws->lut[i - 1]) * 256U;
                    ws->scratch[i] = ws->scratch[i - 1] + (step > cap ? cap : step);
                }
                offset = (int64_t)ws->lut[median] * 256U - ws->scratch[median];
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
                    int64_t v = ws->scratch[i] + offset;
                    ws->scratch[i] = (uint32_t)(v < 0 ? 0 : v > 16383 * 256 ? 16383 * 256 : v);
                }
            } else {
                for (i = 0; i < AGC_WEIGHTED_BINS; i++) ws->scratch[i] = ws->lut[i] * 256U;
            }
            low_value = ws->scratch[knee]; upper_value = ws->scratch[last];
            hot_curve = upper_value > low_value;
        }
    }
    /* Exponential toe joins the linear segment with the same value/slope.
     * Two exp calls per LUT; recurrence avoids an exp for every input DN. */
    shadow_span = target - 16.0;
    shadow_value = shadow_span * exp(-gain * center / shadow_span);
    shadow_step = exp(gain / shadow_span);
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
        double gray = target + gain * (i - center);
        if (i < center) {
            gray = 16.0 + shadow_value;
            shadow_value *= shadow_step;
        }
        if (hot_curve && i >= knee) {
            double fraction = (ws->scratch[i] - low_value) / (upper_value - low_value);
            double anchor = target + gain * (knee - center);
            gray = anchor + (245.0 - anchor) * fmin(1.0, fmax(0.0, fraction));
        }
        ws->lut[i] = (uint16_t)floor(fmin(16383.0, fmax(0.0, gray * 16383.0 / 255.0)) + 0.5);
    }
    return 0;
}

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
        ws->last_params.temporal_previous_percent != p->temporal_previous_percent ||
        ws->last_params.balanced_background != p->balanced_background) {
        /* The first fresh frame can also establish size/parameter caches. */
        uint32_t reacquire = ws->reacquire_remaining;
        agc_weighted_state_reset(ws);
        ws->reacquire_remaining = reacquire;
    }
    if (!p->balanced_background) ws->reacquire_remaining = 0;
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
    if (p->balanced_background) {
        if (balanced_build_lut(ws, p, pixels)) return -1;
        goto temporal;
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
    if (p->balanced_background) {
        ws->gain_offset_valid = 0;
        if (p->gain_limit_tenths) {
            uint32_t cap = (uint32_t)round_even((uint64_t)p->gain_limit_tenths * 16383U * 256U, 2550U);
            uint32_t previous = ws->scratch[0];
            for (i = 1; i < AGC_WEIGHTED_BINS; i++) {
                uint32_t current = ws->scratch[i], step = current - previous;
                previous = current;
                ws->scratch[i] = ws->scratch[i - 1] + (step > cap ? cap : step);
            }
        }
    } else if (p->gain_limit_tenths) {
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
    printf("Balanced Background v2: %s (low-temperature group, protected shadows >=16 Gray; CFG agc_weighted_balanced_background; no DDE).\n",
        p.balanced_background ? "on" : "off");
    printf("Gain Limit: 0 off; %s; no rescaling.\n", p.balanced_background ?
        "anchored background/hot curve, no global brightness compensation" : "mean-brightness compensation with temporal smoothing");
    printf("LUT / Gain brightness smoothing: previous %u%% / current %u%% (0 previous: off)\n",
        p.temporal_previous_percent, 100U - p.temporal_previous_percent);
    if (p.balanced_background)
        printf("Background tracking when Smooth>0: hold hot fallback; 3-frame confirmation; max1 DN center / 0.5 Gray target per frame.\n");
    printf("Settings: CFG Setting -> AGC setting -> Weighted Mapping\n0. Back\n>> ");
    (void)fgets(buf, sizeof(buf), stdin);
    return 0;
}
