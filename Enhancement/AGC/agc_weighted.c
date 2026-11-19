#include "agc_weighted.h"
#include "agc_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define WEIGHTED_FAST_NEON 1
#else
#define WEIGHTED_FAST_NEON 0
#endif

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
        p->temporal_previous_percent > 99 || p->simple_mode > AGC_WEIGHTED_LOCAL_MEAN ||
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

void agc_weighted_workspace_release(AGC_WEIGHTED_WORKSPACE *ws)
{
    if (!ws) return;
    free(ws->local_buffer);
    ws->local_buffer = NULL;
    ws->local_capacity_pixels = 0;
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
static double histogram_percentile(const uint32_t *h, uint64_t pixels,
    uint32_t numerator, uint32_t denominator)
{
    uint64_t rank = (pixels - 1U) * numerator, index = rank / denominator;
    uint32_t rem = (uint32_t)(rank % denominator), low = order_dn(h, index);
    uint32_t high = order_dn(h, index + (rem != 0));
    return low + (high - low) * (rem / (double)denominator);
}

/* Shared Classic interval-weight LUT builder. Only populated bins within the
 * supplied range participate in Clip/Redistribution and the retained percentile.
 * No temporal blend or Gain Limit here: those run once after the full LUT.
 */
static int weighted_build_lut(AGC_WEIGHTED_WORKSPACE *ws,
    const AGC_WEIGHTED_PARAMS *p, uint64_t pixels,
    uint32_t first, uint32_t last, uint32_t unique)
{
    uint32_t i, threshold;
    uint64_t processed_pixels = pixels;
    const uint32_t *counts = ws->histogram;

    memset(ws->lut, 0, sizeof(ws->lut));
    if (unique <= 1) return 0; /* Unsmoothed constant mapping is zero. */
    if (p->clip_tenths) {
        uint64_t cap64 = round_even(pixels * p->clip_tenths, (uint64_t)unique * 10U);
        uint32_t cap = cap64 > pixels ? (uint32_t)pixels : (uint32_t)cap64;
        uint64_t excess = 0, capacity = 0;
        counts = ws->scratch;
        if (!cap) cap = 1;
        for (i = first; i <= last; i++) {
            uint32_t original = ws->histogram[i];
            uint32_t retained = original < cap ? original : cap;
            ws->scratch[i] = retained;
            excess += original - retained;
            if (p->redistribute && original) capacity += cap - retained;
        }
        processed_pixels -= excess;
        if (p->redistribute) {
            if (excess > capacity) excess = capacity;
            while (excess) {
                uint32_t available = 0, at = 0;
                uint32_t remainder, stride = 0, stride_rem = 0, next = 0, error = 0;
                uint64_t batch, added = 0;
                for (i = first; i <= last; i++) available += ws->histogram[i] != 0 && ws->scratch[i] < cap;
                if (!available) break;
                batch = excess / available; remainder = (uint32_t)(excess % available);
                if (remainder) {
                    stride = available / remainder;
                    stride_rem = available % remainder;
                } else {
                    next = available;
                }
                for (i = first; i <= last; i++) if (ws->histogram[i] && ws->scratch[i] < cap) {
                    uint64_t inc = batch, room = cap - ws->scratch[i];
                    if (inc > room) inc = room;
                    if (at == next) {
                        if (inc < room) inc++;
                        /* Advance floor(k * available / remainder) without per-bin division. */
                        next += stride;
                        error += stride_rem;
                        if (error >= remainder) { next++; error -= remainder; }
                    }
                    ws->scratch[i] += (uint32_t)inc; added += inc; at++;
                }
                if (!added) return -1;
                excess -= added;
                processed_pixels += added;
            }
        }
    }
    if (!processed_pixels) return 0;
    if (p->bright_weight == 1.0) {
        /* Endpoints occur once, interior counts twice. The common 100 cancels. */
        uint64_t total = 2U * processed_pixels - counts[first] - counts[last], cumulative = 0;
        for (i = first; i < last; i++) {
            uint64_t weight = (uint64_t)counts[i] + counts[i + 1];
            cumulative += weight;
            ws->lut[i + 1] = weight ? (uint16_t)round_even(cumulative * 16383U, total) : ws->lut[i];
        }
    } else {
        uint64_t rank = processed_pixels * p->bright_percentile_tenths / 1000U;
        double bright = p->bright_weight, sum = 0, total_float;
        double base_weight = (double)(2U * processed_pixels - counts[first] - counts[last]);
        int scaled;
        if (rank >= processed_pixels) rank = processed_pixels - 1;
        /* Clipped scratch is only initialized within [first, last]. */
        threshold = first;
        while (rank >= counts[threshold]) rank -= counts[threshold++];
        /* Common scaling matches Toolkit and keeps finite multipliers safe. */
        scaled = bright > DBL_MAX / fmax(1.0, 512.0 * base_weight);
        total_float = weight_sum_float(counts, first, last - first, threshold, bright, scaled);
        if (total_float <= 0) return 0;
        for (i = first; i < last; i++) {
            double mapped, fraction;
            uint32_t q;
            sum += interval_weight_float(counts, i, threshold, bright, scaled);
            mapped = sum * 16383.0 / total_float;
            q = (uint32_t)mapped;
            fraction = mapped - q;
            q += fraction > 0.5 || (fraction == 0.5 && (q & 1U));
            ws->lut[i + 1] = (uint16_t)(q > 16383 ? 16383 : q);
        }
    }
    for (i = last + 1; i < AGC_WEIGHTED_BINS; i++) ws->lut[i] = 16383;
    return 0;
}

/* BORDER_REFLECT_101, including dimensions smaller than the radius. */
static uint32_t local_reflect(int64_t position, uint32_t length)
{
    int64_t period;
    if (length <= 1U) return 0;
    /* Our caller offsets fit in int64_t and never reach INT64_MIN. */
    if (position < 0) position = -position;
    if (position < length) return (uint32_t)position;
    period = 2 * (int64_t)length - 2;
    if (position >= period) position %= period;
    return (uint32_t)(position < length ? position : period - position);
}

/* Sliding sums. src/dst must be distinct. */
static void local_box(const float *src, float *dst, float *temporary,
    uint32_t width, uint32_t height, int radius)
{
    enum { columns = 64, max_window = 49 };
    const uint32_t r = (uint32_t)radius;
    const double inverse = 1.0 / (2 * radius + 1);
    const uint32_t interior_end = width > r + 1U ? width - r - 1U : 0U;
    uint32_t first_cols[max_window];
    size_t first_rows[max_window];
    uint32_t x, y, start;
    int k;
    for (k = -radius; k <= radius; k++) {
        first_cols[k + radius] = local_reflect(k, width);
        first_rows[k + radius] = (size_t)local_reflect(k, height) * width;
    }
    for (y = 0; y < height; y++) {
        const float *row = src + (size_t)y * width;
        float *out = temporary + (size_t)y * width;
        double sum = 0;
        for (k = 0; k <= 2 * radius; k++) sum += row[first_cols[k]];
        for (x = 0; x < r && x < width - 1U; x++) {
            out[x] = (float)(sum * inverse);
            sum += (double)row[local_reflect((int64_t)x + radius + 1, width)] -
                row[local_reflect((int64_t)x - radius, width)];
        }
        for (; x < interior_end; x++) {
            out[x] = (float)(sum * inverse);
            sum += (double)row[x + radius + 1] - row[x - radius];
        }
        for (; x < width - 1U; x++) {
            out[x] = (float)(sum * inverse);
            sum += (double)row[local_reflect((int64_t)x + radius + 1, width)] - row[x - radius];
        }
        out[x] = (float)(sum * inverse);
    }
    /* Walk contiguous row segments with one running sum per column. The
     * strip bounds only scratch usage; windows still span the whole image. */
    for (start = 0; start < width; ) {
        double sums[columns] = {0};
        uint32_t n = width - start < columns ? width - start : columns;
        for (k = 0; k <= 2 * radius; k++) {
            const float *row = temporary + first_rows[k] + start;
            for (x = 0; x < n; x++) sums[x] += row[x];
        }
        for (x = 0; x < n; x++) dst[start + x] = (float)(sums[x] * inverse);
        for (y = 1; y < height; y++) {
            const float *add = temporary + (size_t)local_reflect((int64_t)y + radius, height) * width + start;
            const float *sub = temporary + (size_t)local_reflect((int64_t)y - radius - 1, height) * width + start;
            float *out = dst + (size_t)y * width + start;
            for (x = 0; x < n; x++) {
                sums[x] += (double)add[x] - sub[x];
                out[x] = (float)(sums[x] * inverse);
            }
        }
        start += n;
    }
}

static int local_prepare(AGC_WEIGHTED_WORKSPACE *ws, size_t count)
{
    float *next;
    if (ws->local_buffer && ws->local_capacity_pixels >= count) return 0;
    if (count > SIZE_MAX / (4U * sizeof(float))) return -1;
    next = (float *)realloc(ws->local_buffer, count * 4U * sizeof(float));
    if (!next) return -1;
    ws->local_buffer = next;
    ws->local_capacity_pixels = count;
    return 0;
}

static double brightness_curve(double gray)
{
    static const double x[] = {0,12,28,60,90,130,170,200,255};
    static const double y[] = {0,22,48,89,115,143,170,200,255};
    unsigned int i;
    gray = 5.0 + 206.0 * gray / 255.0;
    for (i = 1; i < sizeof(x) / sizeof(x[0]); i++)
        if (gray <= x[i]) return y[i-1] + (y[i]-y[i-1]) * (gray-x[i-1]) / (x[i]-x[i-1]);
    return y[sizeof(y) / sizeof(y[0]) - 1];
}

static uint16_t brightness_lut[AGC_WEIGHTED_BINS];
static pthread_once_t brightness_once = PTHREAD_ONCE_INIT;

static void init_brightness_lut(void)
{
    uint32_t i;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++)
        brightness_lut[i] = (uint16_t)floor(brightness_curve(i * (255.0 / 16383.0)) *
            (16383.0 / 255.0) + 0.5);
}

static double local_center(const AGC_WEIGHTED_WORKSPACE *ws, size_t count)
{
    uint32_t i;
    double sum = 0;
    for (i = 0; i < AGC_WEIGHTED_BINS; i++) sum += (double)i * ws->histogram[i];
    return sum / count;
}

static double local_span(const AGC_WEIGHTED_WORKSPACE *ws, size_t count)
{
    return fmax(32.0, histogram_percentile(ws->histogram, count, 99, 100) -
        histogram_percentile(ws->histogram, count, 1, 100));
}

static void local_coefficients(float *mean, float *a, float *b, float *tmp,
    uint32_t width, uint32_t height, int radius, double span)
{
    size_t index, count = (size_t)width * height;
    double epsilon = fmax(2.0, 0.12 * span), epsilon2 = epsilon * epsilon;
    local_box(a, mean, tmp, width, height, radius);
    local_box(b, a, tmp, width, height, radius);
    for (index = 0; index < count; index++) {
        double variance = a[index] - (double)mean[index] * mean[index];
        if (variance < 0.0) variance = 0.0;
        double slope = variance / (variance + epsilon2);
        a[index] = (float)slope;
        b[index] = (float)(mean[index] - slope * mean[index]);
    }
    local_box(a, mean, tmp, width, height, radius);
    local_box(b, a, tmp, width, height, radius);
}

static int output_local(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws, const AGC_WEIGHTED_PARAMS *p)
{
    size_t count = (size_t)width * height, index;
    float *mean = ws->local_buffer, *a = mean + count, *b = a + count, *tmp = b + count;
    double center = local_center(ws, count), span = local_span(ws, count), gain, limit;
    uint32_t x, y;
    gain = p->simple_mode == 2 ? fmin(0.8, 510.0 / span) : fmin(1.2, 765.0 / span);
    limit = p->simple_mode == 2 ? 32.0 : 44.0;
    for (y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        for (x = 0; x < width; x++) {
            double value = (row[x] > 16383U ? 16383U : row[x]) - center;
            index = (size_t)y * width + x;
            a[index] = (float)value;
            b[index] = (float)(value * value);
        }
    }
    local_coefficients(mean, a, b, tmp, width, height, 24, span);
    /* All neighbour reads finish before output writes: supports input==output. */
    for (y = 0; y < height; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *out = (uint16_t *)((uint8_t *)output + y * output_stride);
        for (x = 0; x < width; x++) {
            uint32_t dn = in[x] > 16383U ? 16383U : in[x];
            double smooth, correction, gray;
            index = (size_t)y * width + x;
            smooth = mean[index] * (dn - center) + a[index] + center;
            correction = gain * (dn - smooth);
            if (correction < -limit) correction = -limit;
            if (correction > limit) correction = limit;
            gray = ws->lut[dn] * (255.0 / 16383.0) + correction;
            if (gray < 3.0) gray = 3.0;
            if (gray > 246.0) gray = 246.0;
            /* Positive and inside RAW14 range, so truncation is floor. */
            out[x] = (uint16_t)(gray * (16383.0 / 255.0) + 0.5);
        }
    }
    return 0;
}

static uint32_t local_grid_size(uint32_t length) { return length / 4U + (length % 4U != 0); }

/* The guide samples are centered on 4x4 blocks, at 1.5, 5.5, ... .
 * Clamp interpolation to the outermost samples. */
static float local_grid_position(uint32_t pixel, uint32_t length, uint32_t *lo, uint32_t *hi)
{
    uint32_t right = pixel / 4U + (pixel % 4U >= 2U);
    *lo = right ? right - 1U : 0;
    *hi = right < length ? right : length - 1U;
    return 0.125f + 0.25f * ((pixel + 2U) % 4U);
}

static inline uint16_t local_fast_pixel(uint16_t input, const uint16_t *lut,
    float slope, float offset, float center, float gain)
{
    const float limit = 32.0f * (16383.0f / 255.0f);
    const float min = 3.0f * (16383.0f / 255.0f), max = 246.0f * (16383.0f / 255.0f);
    uint32_t dn = input > 16383U ? 16383U : input;
    float correction = gain * ((1.0f - slope) * (dn - center) - offset);
    float mapped;
    if (correction < -limit) correction = -limit;
    if (correction > limit) correction = limit;
    mapped = lut[dn] + correction;
    if (mapped < min) mapped = min;
    if (mapped > max) mapped = max;
    return (uint16_t)(mapped + 0.5f);
}

static inline void local_fast_four(const uint16_t *in, uint16_t *out, const uint16_t *lut,
    float al, float ah, float bl, float bh, float center, float gain)
{
#if WEIGHTED_FAST_NEON
    const float32x4_t fx = {0.125f, 0.375f, 0.625f, 0.875f};
    const float limit = 32.0f * (16383.0f / 255.0f);
    uint16x4_t dn = vmin_u16(vld1_u16(in), vdup_n_u16(16383U));
    /* NEON has no gather for the RAW14 LUT. */
    uint16x4_t base = vdup_n_u16(lut[vget_lane_u16(dn, 0)]);
    base = vset_lane_u16(lut[vget_lane_u16(dn, 1)], base, 1);
    base = vset_lane_u16(lut[vget_lane_u16(dn, 2)], base, 2);
    base = vset_lane_u16(lut[vget_lane_u16(dn, 3)], base, 3);
    float32x4_t slope = vaddq_f32(vdupq_n_f32(al), vmulq_n_f32(fx, ah - al));
    float32x4_t offset = vaddq_f32(vdupq_n_f32(bl), vmulq_n_f32(fx, bh - bl));
    float32x4_t centered = vsubq_f32(vcvtq_f32_u32(vmovl_u16(dn)), vdupq_n_f32(center));
    float32x4_t scale = vsubq_f32(vdupq_n_f32(1.0f), slope);
    float32x4_t correction = vmulq_n_f32(vsubq_f32(vmulq_f32(scale, centered), offset), gain);
    correction = vmaxq_f32(vdupq_n_f32(-limit), vminq_f32(vdupq_n_f32(limit), correction));
    float32x4_t mapped = vaddq_f32(vcvtq_f32_u32(vmovl_u16(base)), correction);
    mapped = vmaxq_f32(vdupq_n_f32(3.0f * (16383.0f / 255.0f)),
        vminq_f32(vdupq_n_f32(246.0f * (16383.0f / 255.0f)), mapped));
    vst1_u16(out, vmovn_u32(vcvtq_u32_f32(vaddq_f32(mapped, vdupq_n_f32(0.5f)))));
#else
    unsigned int j;
    for (j = 0; j < 4; j++) {
        float fx = 0.125f + 0.25f * j;
        out[j] = local_fast_pixel(in[j], lut, al + fx * (ah - al),
            bl + fx * (bh - bl), center, gain);
    }
#endif
}

#if WEIGHTED_FAST_NEON
static inline void local_store_moments(float *a, float *b, uint32_t sum, uint32_t squares,
    double inverse, double center)
{
    double raw_mean = sum * inverse;
    *a = (float)(raw_mean - center);
    *b = (float)(squares * inverse - center * (2.0 * raw_mean - center));
}
#endif

static void local_fast_pool(const uint16_t *input, size_t stride, uint32_t width,
    uint32_t height, float *a, float *b, double center)
{
    uint32_t w = local_grid_size(width), h = local_grid_size(height), y;
    for (y = 0; y < h; y++, a += w, b += w) {
        const uint8_t *base = (const uint8_t *)input + (size_t)y * 4U * stride;
        uint32_t ny = height - y * 4U, x = 0;
        if (ny > 4U) ny = 4U;
#if WEIGHTED_FAST_NEON
        if (ny >= 4U) {
            const uint16_t *r0 = (const uint16_t *)base;
            const uint16_t *r1 = (const uint16_t *)(base + stride);
            const uint16_t *r2 = (const uint16_t *)(base + 2U * stride);
            const uint16_t *r3 = (const uint16_t *)(base + 3U * stride);
            const uint16x8_t limit = vdupq_n_u16(16383U);
            for (; x < width / 8U * 2U; x += 2U) {
                uint32_t col = x * 4U;
                uint16x8_t d0 = vminq_u16(vld1q_u16(r0 + col), limit);
                uint16x8_t d1 = vminq_u16(vld1q_u16(r1 + col), limit);
                uint16x8_t d2 = vminq_u16(vld1q_u16(r2 + col), limit);
                uint16x8_t d3 = vminq_u16(vld1q_u16(r3 + col), limit);
                uint32x4_t sum_pairs = vpaddlq_u16(vaddq_u16(vaddq_u16(d0, d1), vaddq_u16(d2, d3)));
                uint32x2_t sums = vpadd_u32(vget_low_u32(sum_pairs), vget_high_u32(sum_pairs));
                uint32x4_t sq0 = vmull_u16(vget_low_u16(d0), vget_low_u16(d0));
                uint32x4_t sq1 = vmull_u16(vget_high_u16(d0), vget_high_u16(d0));
                sq0 = vmlal_u16(sq0, vget_low_u16(d1), vget_low_u16(d1));
                sq1 = vmlal_u16(sq1, vget_high_u16(d1), vget_high_u16(d1));
                sq0 = vmlal_u16(sq0, vget_low_u16(d2), vget_low_u16(d2));
                sq1 = vmlal_u16(sq1, vget_high_u16(d2), vget_high_u16(d2));
                sq0 = vmlal_u16(sq0, vget_low_u16(d3), vget_low_u16(d3));
                sq1 = vmlal_u16(sq1, vget_high_u16(d3), vget_high_u16(d3));
                uint32x2_t squares = vpadd_u32(
                    vpadd_u32(vget_low_u32(sq0), vget_high_u32(sq0)),
                    vpadd_u32(vget_low_u32(sq1), vget_high_u32(sq1)));
                local_store_moments(a + x, b + x, vget_lane_u32(sums, 0), vget_lane_u32(squares, 0), 1.0 / 16.0, center);
                local_store_moments(a + x + 1U, b + x + 1U, vget_lane_u32(sums, 1), vget_lane_u32(squares, 1), 1.0 / 16.0, center);
            }
        }
#endif
        for (; x < w; x++) {
            uint32_t xs = x * 4U, nx = width - xs < 4U ? width - xs : 4U;
            uint32_t j, k, sum = 0, squares = 0;
            double raw_mean, raw_square_mean;
            for (j = 0; j < ny; j++) {
                const uint16_t *row = (const uint16_t *)(base + j * stride) + xs;
                for (k = 0; k < nx; k++) {
                    uint32_t dn = row[k] > 16383U ? 16383U : row[k];
                    sum += dn;
                    squares += dn * dn; /* 16 * 16383^2 fits uint32_t. */
                }
            }
            if (nx == 4U && ny == 4U) {
                raw_mean = sum * (1.0 / 16.0);
                raw_square_mean = squares * (1.0 / 16.0);
            } else {
                raw_mean = sum / (double)(nx * ny);
                raw_square_mean = squares / (double)(nx * ny);
            }
            a[x] = (float)(raw_mean - center);
            b[x] = (float)(raw_square_mean - center * (2.0 * raw_mean - center));
        }
    }
}

static int output_local_fast(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws)
{
    uint32_t w = local_grid_size(width), h = local_grid_size(height), x, y;
    size_t grid_count = (size_t)w * h, pixels = (size_t)width * height;
    float *mean = ws->local_buffer, *a = mean + grid_count, *b = a + grid_count;
    const double center = local_center(ws, pixels), span = local_span(ws, pixels);
    const float center_f = (float)center;
    const float gain_dn = (float)(fmin(0.8, 510.0 / span) * (16383.0 / 255.0));
    local_fast_pool(input, input_stride, width, height, a, b, center);
    local_coefficients(mean, a, b, b + grid_count, w, h, 6, span);
    for (y = 0; y < height; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *out = (uint16_t *)((uint8_t *)output + y * output_stride);
        uint32_t lo, hi;
        float fy = local_grid_position(y, h, &lo, &hi);
        const float *a0 = mean + (size_t)lo * w, *a1 = mean + (size_t)hi * w;
        const float *b0 = a + (size_t)lo * w, *b1 = a + (size_t)hi * w;
        float al = a0[0] + fy * (a1[0] - a0[0]);
        float bl = b0[0] + fy * (b1[0] - b0[0]);
        for (x = 0; x < 2U && x < width; x++)
            out[x] = local_fast_pixel(in[x], ws->lut, al, bl, center_f, gain_dn);
        /* Reuse each vertically interpolated endpoint in the next interval. */
        for (hi = 1; hi < w && width - x >= 4U; hi++, x += 4U) {
            float ah = a0[hi] + fy * (a1[hi] - a0[hi]);
            float bh = b0[hi] + fy * (b1[hi] - b0[hi]);
            local_fast_four(in + x, out + x, ws->lut, al, ah, bl, bh, center_f, gain_dn);
            al = ah; bl = bh;
        }
        if (x < width) {
            float ah = hi < w ? a0[hi] + fy * (a1[hi] - a0[hi]) : al;
            float bh = hi < w ? b0[hi] + fy * (b1[hi] - b0[hi]) : bl;
            float fx = 0.125f;
            for (; x < width; x++, fx += 0.25f)
                out[x] = local_fast_pixel(in[x], ws->lut, al + fx * (ah - al),
                    bl + fx * (bh - bl), center_f, gain_dn);
        }
    }
    return 0;
}

static inline uint16_t local_mean_pixel(uint16_t input, const uint16_t *lut,
    int32_t background_q6, int32_t gain_q8)
{
    const int32_t limit = (int32_t)(32.0 * 16383.0 / 255.0 * 256.0 + 0.5);
    const int32_t min = (int32_t)(3.0 * 16383.0 / 255.0 * 256.0 + 0.5);
    const int32_t max = (int32_t)(246.0 * 16383.0 / 255.0 * 256.0 + 0.5);
    uint32_t dn = input > 16383U ? 16383U : input;
    int32_t detail = (int32_t)dn - ((background_q6 + 32) >> 6);
    int32_t correction = gain_q8 * detail, mapped;
    if (correction < -limit) correction = -limit;
    if (correction > limit) correction = limit;
    mapped = 256 * (int32_t)lut[dn] + correction;
    if (mapped < min) mapped = min;
    if (mapped > max) mapped = max;
    return (uint16_t)((mapped + 128) >> 8);
}

static int output_local_mean(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws)
{
    uint32_t w = local_grid_size(width), h = local_grid_size(height), x, y;
    size_t i, n = (size_t)w * h;
    float *pooled = ws->local_buffer, *mean = pooled + n, *tmp = mean + n;
    double span = local_span(ws, (size_t)width * height);
    int32_t gain = (int32_t)(fmin(0.8, 510.0 / span) * (16383.0 / 255.0) * 256.0 + 0.5);

    for (y = 0; y < h; y++) {
        uint32_t ys = y * 4U, ny = height - ys < 4U ? height - ys : 4U;
        const uint8_t *base = (const uint8_t *)input + ys * input_stride;
        for (x = 0; x < w; x++) {
            uint32_t xs = x * 4U, nx = width - xs < 4U ? width - xs : 4U;
            uint32_t j, k, sum = 0;
            for (j = 0; j < ny; j++) {
                const uint16_t *row = (const uint16_t *)(base + j * input_stride) + xs;
                for (k = 0; k < nx; k++) sum += row[k] > 16383U ? 16383U : row[k];
            }
            pooled[(size_t)y * w + x] = nx == 4U && ny == 4U ?
                sum * (1.0f / 16.0f) : sum / (float)(nx * ny);
        }
    }
    local_box(pooled, mean, tmp, w, h, 6);
    /* Rounded DN values are exact in the shared float workspace. */
    for (i = 0; i < n; i++) pooled[i] = (float)(uint32_t)(mean[i] + 0.5f);
    for (y = 0; y < height; y++) {
        const uint16_t *in = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        uint16_t *out = (uint16_t *)((uint8_t *)output + y * output_stride);
        uint32_t lo, hi;
        int32_t fy = (int32_t)(8.0f * local_grid_position(y, h, &lo, &hi));
        const float *row0 = pooled + (size_t)lo * w, *row1 = pooled + (size_t)hi * w;
        int32_t left = (8 - fy) * (int32_t)row0[0] + fy * (int32_t)row1[0];
        for (x = 0; x < 2U && x < width; x++)
            out[x] = local_mean_pixel(in[x], ws->lut, 8 * left, gain);
        for (hi = 1; hi < w && x < width; hi++) {
            int32_t right = (8 - fy) * (int32_t)row0[hi] + fy * (int32_t)row1[hi];
            int32_t background = 7 * left + right, step = 2 * (right - left);
            uint32_t j, count = width - x < 4U ? width - x : 4U;
            for (j = 0; j < count; j++, background += step)
                out[x + j] = local_mean_pixel(in[x + j], ws->lut, background, gain);
            x += count;
            left = right;
        }
        for (; x < width; x++)
            out[x] = local_mean_pixel(in[x], ws->lut, 8 * left, gain);
    }
    return 0;
}

int agc_weighted_apply_u14(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *ws, const AGC_WEIGHTED_PARAMS *p)
{
    uint32_t x, y, i, first = 0, last = 0, unique = 0, offset_q8 = 0;
    uint64_t pixels = (uint64_t)width * height;
    if (!input || !output || !ws || !width || !height || pixels > UINT32_MAX ||
        agc_weighted_validate_params(p) || input_stride < (size_t)width * 2 ||
        output_stride < (size_t)width * 2 || (input_stride | output_stride) & 1U ||
        height > SIZE_MAX / input_stride || height > SIZE_MAX / output_stride) return -1;
    if (p->simple_mode >= 2 && input == output && input_stride != output_stride) return -1;
    if (p->simple_mode >= 2) {
        size_t local_pixels = p->simple_mode >= AGC_WEIGHTED_FAST_LOCAL ?
            (size_t)local_grid_size(width) * local_grid_size(height) : (size_t)pixels;
        if (local_prepare(ws, local_pixels)) return -1;
    }
    if (!ws->last_width || ws->last_params.simple_mode != p->simple_mode) {
        printf("Weighted Brightness/Local: mode=%u (0 Off, 1 Brightness, 2 Mild, 3 Strong, 4 Fast Local, 5 Local Mean); "
            "retained spatial scratch=%zu bytes; RAW14 output\n", (unsigned)p->simple_mode,
            ws->local_capacity_pixels * 4U * sizeof(float));
        if (p->simple_mode == AGC_WEIGHTED_FAST_LOCAL)
            printf("Weighted Fast Local: backend=%s\n", WEIGHTED_FAST_NEON ? "NEON" : "scalar");
    }
    if (ws->last_width != width || ws->last_height != height ||
        ws->last_params.gain_limit_tenths != p->gain_limit_tenths ||
        ws->last_params.temporal_previous_percent != p->temporal_previous_percent ||
        ws->last_params.simple_mode != p->simple_mode ||
        ws->last_params.clip_tenths != p->clip_tenths ||
        ws->last_params.redistribute != p->redistribute ||
        ws->last_params.bright_weight != p->bright_weight ||
        ws->last_params.bright_percentile_tenths != p->bright_percentile_tenths)
        agc_weighted_state_reset(ws);
    memset(ws->histogram, 0, sizeof(ws->histogram));
    for (y = 0; y < height; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)input + y * input_stride);
        for (x = 0; x < width; x++) ws->histogram[row[x] > 16383U ? 16383U : row[x]]++;
    }

    for (i = 0; i < AGC_WEIGHTED_BINS; i++) if (ws->histogram[i]) {
        if (!unique) first = i;
        last = i; unique++;
    }
    if (weighted_build_lut(ws, p, pixels, first, last, unique)) return -1;

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
        uint32_t limit_q8 = (uint32_t)round_even(
            (uint64_t)p->gain_limit_tenths * 16383U * 256U, 2550U);
        uint32_t previous = ws->scratch[0], limited = 0;
        difference_sum = (uint64_t)ws->histogram[0] * previous;
        ws->scratch[0] = 0;
        /* Accumulate the histogram-weighted mean lost to slope limiting
         * rather than using a discrete DN anchor that can flicker. */
        for (i = 1; i < AGC_WEIGHTED_BINS; i++) {
            uint32_t current = ws->scratch[i];
            /* Monotone LUT: delta >= 0, so min(delta, limit) needs no max(delta, 0). */
            uint32_t step = current - previous;
            previous = current;
            if (step > limit_q8) step = limit_q8;
            limited += step;
            ws->scratch[i] = limited;
            difference_sum += (uint64_t)ws->histogram[i] * (current - limited);
        }
        offset_q8 = (uint32_t)((difference_sum + pixels / 2U) / pixels);
        if (ws->gain_offset_valid && p->temporal_previous_percent)
            offset_q8 = (offset_q8 * (100U - p->temporal_previous_percent) +
                ws->gain_offset_q8 * p->temporal_previous_percent + 50U) / 100U;
        ws->gain_offset_q8 = offset_q8;
        ws->gain_offset_valid = 1;
    } else {
        ws->gain_offset_valid = 0;
    }
    if (p->simple_mode) {
        pthread_once(&brightness_once, init_brightness_lut);
        for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
            uint32_t dn = (ws->scratch[i] + offset_q8 + 128U) / 256U;
            ws->lut[i] = brightness_lut[dn > 16383U ? 16383U : dn];
        }
        if (p->simple_mode == AGC_WEIGHTED_FAST_LOCAL)
            return output_local_fast(input, input_stride, output, output_stride, width, height, ws);
        if (p->simple_mode == AGC_WEIGHTED_LOCAL_MEAN)
            return output_local_mean(input, input_stride, output, output_stride, width, height, ws);
        if (p->simple_mode >= 2)
            return output_local(input, input_stride, output, output_stride, width, height, ws, p);
    } else for (i = 0; i < AGC_WEIGHTED_BINS; i++) {
        uint32_t dn = (ws->scratch[i] + offset_q8 + 128U) / 256U;
        ws->lut[i] = (uint16_t)(dn > 16383U ? 16383U : dn);
    }
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
    printf("Brightness + Local Contrast: %u (0 Off, 1 Brightness, 2 Mild, 3 Strong, 4 Fast Local, 5 Local Mean).\n", (unsigned)p.simple_mode);
    printf("Gain Limit: 0 off; mean-brightness compensation; optional brightness/local correction follows.\n");
    printf("LUT/Gain Smooth: previous %u%%; local correction has no pixel history.\n", (unsigned)p.temporal_previous_percent);
    printf("Settings: CFG Setting -> AGC setting -> Weighted Mapping\n0. Back\n>> ");
    (void)fgets(buf, sizeof(buf), stdin);
    return 0;
}
