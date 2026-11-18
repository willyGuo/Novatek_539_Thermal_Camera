#include "Enhancement/RAW/raw_temporal_denoise.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RAW_TEMPORAL_TEST_ALLOCATOR
static int fail_after = -1, allocations;

void *raw_test_malloc(size_t size)
{
    void *p;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    p = malloc(size);
    if (p) ++allocations;
    return p;
}

void raw_test_free(void *p)
{
    if (p) --allocations;
    free(p);
}

static void test_allocation_failure(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    uint16_t a[25], before[25];
    uint32_t *fast, *slow;
    uint16_t *motion, *row;
    unsigned i;
    int failure;
    p.enable = p.spatial_enable = p.dual_speed_enable = 1;
    for (i = 0; i < 25; ++i) a[i] = 1000;
    memcpy(before, a, sizeof(a));
    for (failure = 0; failure < 4; ++failure) {
        fail_after = failure;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == RAW_TEMPORAL_ERROR_MEMORY);
        assert(!s.history_q8 && !s.slow_history_q8 && !s.motion && !s.spatial_row && !s.valid);
        assert(!allocations && !memcmp(a, before, sizeof(a)));
    }
    fail_after = -1;
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
    fast = s.history_q8; slow = s.slow_history_q8; motion = s.motion; row = s.spatial_row;
    for (i = 0; i < 25; ++i) a[i] = 2000;
    memcpy(before, a, sizeof(a));
    for (failure = 0; failure < 4; ++failure) {
        fail_after = failure;
        assert(raw_temporal_denoise_process(&s, a, 10, 5, 5, 34000, &p) == RAW_TEMPORAL_ERROR_MEMORY);
        assert(s.history_q8 == fast && s.slow_history_q8 == slow && s.motion == motion && s.spatial_row == row);
        assert(s.valid && s.timestamp_us == 1000 && s.width == 3 && s.height == 3);
        assert(s.capacity == 9 && s.dual_capacity == 9 && s.row_capacity == 3);
        assert(fast[0] == 1000U * 256U && slow[0] == fast[0] && !motion[0]);
        assert(allocations == 4 && !memcmp(a, before, sizeof(a)));
    }
    fail_after = -1;
    assert(raw_temporal_denoise_process(&s, a, 10, 5, 5, 34000, &p) == 2);
    raw_temporal_denoise_destroy(&s);
    assert(!allocations);
    puts("PASS: initial/growth allocation failures preserve pixels/state and leak no buffers");
}
#endif

static void test_spatial(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    uint16_t source[5][7], image[5][7];
    unsigned x, y, trial;
    const unsigned thresholds[] = {5, 8, 10, 12};
    p.enable = p.spatial_enable = 1;
    p.dual_speed_enable = 0;
    p.spatial_mode = 0;
    for (trial = 0; trial < 4; ++trial) {
        p.spatial_threshold_dn = thresholds[trial];
        for (y = 0; y < 5; ++y)
            for (x = 0; x < 7; ++x)
                source[y][x] = x < 5 ? (uint16_t)(65000 + (x * 3 + y * 7) % 13) : 65535;
        memcpy(image, source, sizeof(image));
        assert(raw_temporal_denoise_process(&s, &image[0][0], 14, 5, 5, 1000, &p) == 2);
        for (y = 0; y < 5; ++y) {
            for (x = 0; x < 5; ++x) {
                unsigned c = source[y][x], expected = c * 256U;
                if (y && y < 4 && x && x < 4) {
                    unsigned neighbors[] = {source[y-1][x], source[y+1][x],
                        source[y][x-1], source[y][x+1]};
                    unsigned i, sum = 4U * c, edge = 0;
                    for (i = 0; i < 4; ++i) {
                        unsigned d = c > neighbors[i] ? c - neighbors[i] : neighbors[i] - c;
                        if (d > p.spatial_threshold_dn) edge = 1;
                        sum += neighbors[i];
                    }
                    if (!edge) expected = sum * 32U;
                }
                assert(s.history_q8[y * 5 + x] == expected);
                assert(image[y][x] == (expected + 128U) / 256U);
            }
            assert(image[y][5] == 65535 && image[y][6] == 65535);
        }
    }
    for (trial = 0; trial < 4; ++trial) {
        uint16_t a[9];
        p.spatial_threshold_dn = thresholds[trial];
        for (x = 0; x < 9; ++x) a[x] = 1000;
        a[4] += (uint16_t)thresholds[trial];
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
        assert(s.history_q8[4] == 1000U * 256U + thresholds[trial] * 128U);
        for (x = 0; x < 9; ++x) a[x] = 1000;
        a[4] += (uint16_t)(thresholds[trial] + 1U);
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
        assert(a[4] == 1001 + thresholds[trial]);
    }
    {
        uint16_t a[9];
        for (x = 0; x < 9; ++x) a[x] = 65535;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
        assert(a[4] == 65535 && s.history_q8[4] == 65535U * 256U);
        a[4] = 65534;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 34000, &p) == 1);
        assert(s.history_q8[4] == 65535U * 256U - 32U);
    }
    raw_temporal_denoise_destroy(&s);
    assert(!s.spatial_row && !s.row_capacity);
}

static uint16_t dual_frame(RAW_TEMPORAL_DENOISE_STATE *s,
    RAW_TEMPORAL_DENOISE_PARAMS *p, uint16_t value)
{
    assert(raw_temporal_denoise_process(s, &value, 2, 1, 1,
        s->timestamp_us + 33000U, p) > 0);
    return value;
}

static void test_dual_speed(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    unsigned i;
    p.enable = p.dual_speed_enable = 1;
    assert(dual_frame(&s, &p, 1000) == 1000);
    assert(dual_frame(&s, &p, 1004) == 1000);
    assert(s.history_q8[0] == 1002U * 256U);
    assert(s.slow_history_q8[0] == 1000U * 256U + 64U && s.motion[0] == 0);
    raw_temporal_denoise_reset(&s);
    assert(dual_frame(&s, &p, 1000) == 1000);
    assert(dual_frame(&s, &p, 1007) == 1004);
    assert(s.motion[0] == 0x103 && s.history_q8[0] == s.slow_history_q8[0]);
    for (i = 0; i < 3; ++i) {
        assert(dual_frame(&s, &p, 1004) == 1004);
        assert(s.motion[0] == 0x102 - i);
    }
    assert(dual_frame(&s, &p, 1004) == 1004 && !s.motion[0]);
    raw_temporal_denoise_reset(&s);
    assert(dual_frame(&s, &p, 1000) == 1000);
    assert(dual_frame(&s, &p, 1008) == 1008); /* Residual at exact RAW entry. */
    assert(s.motion[0] == 0x203);
    for (i = 0; i < 3; ++i) {
        assert(dual_frame(&s, &p, 1008) == 1008);
        assert(s.motion[0] == 0x202 - i && s.slow_history_q8[0] == 1008U * 256U);
    }
    assert(dual_frame(&s, &p, 1008) == 1008 && !s.motion[0]);
    assert(dual_frame(&s, &p, 1000) == 1000);
    assert(s.slow_history_q8[0] == 1000U * 256U); /* No trailing old background. */
    p.motion_hold_frames = 0;
    assert(dual_frame(&s, &p, 1000) == 1000);
    assert(dual_frame(&s, &p, 1007) == 1004 && s.motion[0] == 0x100);
    assert(dual_frame(&s, &p, 1009) == 1006 && s.motion[0] == 0x100);
    /* M=2.40625 DN: below entry 3, above exit 2, so remain Fast without hold. */
    assert(s.history_q8[0] == 1006U * 256U + 64U);
    assert(dual_frame(&s, &p, 1015) == 1015 && s.motion[0] == 0x200);
    assert(dual_frame(&s, &p, 1022) == 1022 && s.motion[0] == 0x200);
    /* 7 DN residual is below RAW entry 8 but exactly its hysteresis exit. */
    assert(dual_frame(&s, &p, 1022) == 1022 && !s.motion[0]);
    for (i = 0; i < 8; ++i) {
        RAW_TEMPORAL_DENOISE_PARAMS bad = p;
        uint16_t pixel = 1000;
        if (i == 0) bad.spatial_enable = 2;
        if (i == 1) bad.spatial_threshold_dn = 16384;
        if (i == 2) bad.dual_speed_enable = 2;
        if (i == 3) bad.motion_threshold_dn = 0;
        if (i == 4) bad.motion_strong_threshold_dn = bad.motion_threshold_dn;
        if (i == 5) bad.motion_hysteresis_dn = bad.motion_threshold_dn;
        if (i == 6) bad.motion_hold_frames = 256;
        if (i == 7) bad.motion_strong_threshold_dn = 16384;
        assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 1, &bad) == RAW_TEMPORAL_ERROR_PARAMS);
        assert(pixel == 1000 && s.slow_history_q8[0] == 1022U * 256U);
    }
    assert(dual_frame(&s, &p, 65535) == 65535);
    for (i = 0; i < 10; ++i) assert(dual_frame(&s, &p, 65535) == 65535);
    assert(s.slow_history_q8[0] == 65535U * 256U);
    assert(dual_frame(&s, &p, 65531) == 65535);
    assert(s.slow_history_q8[0] == 65535U * 256U - 64U);
    p.enable = 0;
    {
        uint16_t pixel = 0;
        assert(!raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 1, &p) && !s.valid);
    }
    p.enable = 1;
    assert(dual_frame(&s, &p, 0) == 0 && !s.motion[0] && !s.slow_history_q8[0]);
    raw_temporal_denoise_destroy(&s);
    assert(!s.slow_history_q8 && !s.motion && !s.dual_capacity);
}

static void test_selective_spatial(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    uint16_t source[5][7], image[5][7];
    unsigned trial, x, y;
    p.enable = p.spatial_enable = p.spatial_mode = 1;
    p.dual_speed_enable = 0;
    for (trial = 0; trial < 16; ++trial) {
        unsigned expected = 1000U * 8U;
        const unsigned indices[] = {1, 3, 5, 7};
        uint16_t a[9];
        for (x = 0; x < 9; ++x) a[x] = 1000;
        for (x = 0; x < 4; ++x) {
            a[indices[x]] = (uint16_t)(1000U + ((trial & (1U << x)) ? 8U : 100U));
            if (trial & (1U << x)) expected += 8U;
        }
        raw_temporal_denoise_reset(&s);
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
        assert(s.history_q8[4] == expected * 32U);
        assert(a[4] == (expected + 4U) / 8U);
    }
    /* Independent original-image reference, including padding, full UINT16
     * extremes, accepted negative differences and selective mode reset. */
    for (trial = 0; trial < 4; ++trial) {
        const unsigned bases[] = {0, 1000, 65000, 65527};
        for (y = 0; y < 5; ++y)
            for (x = 0; x < 7; ++x)
                source[y][x] = x < 5 ? (uint16_t)(bases[trial] + (x * 3 + y * 5) % 9) : 65535;
        memcpy(image, source, sizeof(image));
        raw_temporal_denoise_reset(&s);
        assert(raw_temporal_denoise_process(&s, &image[0][0], 14, 5, 5, 1000, &p) == 2);
        for (y = 0; y < 5; ++y) {
            for (x = 0; x < 5; ++x) {
                unsigned c = source[y][x], sum = c * 8U;
                if (y && y < 4 && x && x < 4) {
                    const unsigned n[] = {source[y-1][x], source[y+1][x], source[y][x-1], source[y][x+1]};
                    unsigned i;
                    sum = c * 4U;
                    for (i = 0; i < 4; ++i) {
                        unsigned d = n[i] > c ? n[i] - c : c - n[i];
                        sum += d <= p.spatial_threshold_dn ? n[i] : c;
                    }
                }
                assert(s.history_q8[y*5+x] == sum * 32U);
                assert(image[y][x] == (sum + 4U) / 8U);
            }
            assert(image[y][5] == 65535 && image[y][6] == 65535);
        }
    }
    p.spatial_mode = 2;
    assert(raw_temporal_denoise_set_params(&p) == -1);
    p.spatial_mode = 1;
    p.dual_speed_enable = 1;
    {
        uint16_t a[9];
        for (x = 0; x < 9; ++x) a[x] = 1000;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
        for (x = 0; x < 9; ++x) a[x] = 1016;
        a[4] = 1024;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 34000, &p) == 1);
        assert(a[4] == 1024 && s.motion[4] == 0x203); /* Original RAW on strong motion. */
        p.spatial_mode = 0;
        assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 67000, &p) == 2);
        assert(!s.motion[4]);
    }
    raw_temporal_denoise_destroy(&s);
    puts("PASS: selective spatial 16 neighbor masks, threshold equality, signed differences, original neighbors/padding/UINT16 and strong-motion RAW bypass");
}

static void test_selective_separate_output(void)
{
    enum { HEIGHT = 5, MAX_WIDTH = 640 };
    static const unsigned widths[] = {9, 10, 11, 17, 18, 19, MAX_WIDTH};
    static const unsigned thresholds[] = {0, 8, 16383};
    uint16_t input[HEIGHT][MAX_WIDTH + 3], in_place[HEIGHT][MAX_WIDTH + 3];
    uint16_t output[HEIGHT][MAX_WIDTH + 5];
    RAW_TEMPORAL_DENOISE_STATE scalar = {0}, separate = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    uint64_t timestamp = 1000;
    unsigned i, frame, y, x;

    p.enable = p.spatial_enable = p.spatial_mode = p.dual_speed_enable = 1;
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
        unsigned width = widths[i];
        size_t count = (size_t)width * HEIGHT;
        for (frame = 0; frame < 18; ++frame) {
            int expected, actual;
            p.spatial_threshold_dn = thresholds[frame / 6];
            if (frame == 4) timestamp += 300000;
            for (y = 0; y < HEIGHT; ++y) {
                for (x = 0; x < MAX_WIDTH + 3; ++x) {
                    /* Near saturation, threshold equality, scene jumps, and
                     * signed differences from neighbors on both sides. */
                    unsigned base = frame % 6 < 4 ? 65535U : 32768U;
                    input[y][x] = x < width ? (uint16_t)(base - ((x <= 1 && y <= 1) ? 0 :
                        (x * 7 + y * 3 + frame) % 17)) : 0x7654;
                }
                for (x = 0; x < MAX_WIDTH + 5; ++x) output[y][x] = 0x4567;
            }
            memcpy(in_place, input, sizeof(input));
            expected = raw_temporal_denoise_process(&scalar, &in_place[0][0], sizeof(input[0]),
                width, HEIGHT, timestamp, &p);
            actual = raw_temporal_denoise_process_to(&separate, &input[0][0], sizeof(input[0]),
                &output[0][0], sizeof(output[0]), width, HEIGHT, timestamp, &p);
            assert(expected > 0 && actual == expected);
            for (y = 0; y < HEIGHT; ++y) {
                assert(!memcmp(in_place[y], output[y], width * sizeof(uint16_t)));
                for (x = width; x < MAX_WIDTH + 5; ++x) assert(output[y][x] == 0x4567);
                for (x = width; x < MAX_WIDTH + 3; ++x) assert(input[y][x] == 0x7654);
            }
            assert(!memcmp(scalar.history_q8, separate.history_q8, count * sizeof(uint32_t)));
            assert(!memcmp(scalar.slow_history_q8, separate.slow_history_q8, count * sizeof(uint32_t)));
            assert(!memcmp(scalar.motion, separate.motion, count * sizeof(uint16_t)));
            assert(!memcmp(scalar.spatial_row, separate.spatial_row, width * sizeof(uint16_t)));
            timestamp += 33333;
        }
    }
    raw_temporal_denoise_destroy(&scalar);
    raw_temporal_denoise_destroy(&separate);
    puts("PASS: Selective Spatial + Dual Speed separate output matches scalar across vector widths and tails");
}

static void test_fused_reset(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = RAW_TEMPORAL_DENOISE_DEFAULTS;
    uint16_t a[9];
    unsigned i;
    p.enable=1;
    for (i = 0; i < 9; ++i) a[i] = 1000;
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 1000, &p) == 2);
    for (i = 0; i < 9; ++i) a[i] = 1016;
    a[4] = 1024;
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 34000, &p) == 1);
    assert(a[4] == 1024 && s.history_q8[4] == 1020U * 256U);
    assert(s.motion[4] == 0x203 && s.slow_history_q8[4] == s.history_q8[4]);
    raw_temporal_denoise_reset(&s);
    for (i = 0; i < 9; ++i) a[i] = 1000;
    a[4] = 1001;
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 67000, &p) == 2);
    assert(a[4] == 1001 && s.slow_history_q8[4] == 1000U * 256U + 128U && !s.motion[4]);
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 67000, &p) == 2);
    assert(raw_temporal_denoise_process(&s, a, 6, 3, 3, 317001, &p) == 2);
    assert(raw_temporal_denoise_process(&s, a, 4, 2, 2, 350001, &p) == 2);
    assert(s.width == 2 && s.height == 2);
    p.spatial_enable = 0;
    assert(raw_temporal_denoise_process(&s, a, 4, 2, 2, 383001, &p) == 2);
    raw_temporal_denoise_destroy(&s);
}

int main(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = {.enable = 1, .threshold_dn = 12, .history_shift = 2}, got;
    uint16_t pixel = 9400;
    uint32_t history;
    unsigned i;
    raw_temporal_denoise_get_params(&got);
    assert(got.enable == 0 && got.spatial_enable == 1 && got.dual_speed_enable == 1 &&
        got.threshold_dn == 12 && got.history_shift == 2);
    assert(raw_temporal_denoise_set_params(&p) == 0);
    raw_temporal_denoise_get_params(&got); assert(!memcmp(&p, &got, sizeof(p)));
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 1000, &p) == 2 && pixel == 9400);
    pixel = 9404;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 34000, &p) == 1 && pixel == 9401);
    pixel = 9398;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 67000, &p) == 1 && pixel == 9400);
    assert(s.history_q8[0] == 9400U * 256 + 64); /* Preserve 0.25 DN internally. */
    pixel = 9500;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 100000, &p) == 1 && pixel == 9500);
    pixel = 16384;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 133000, &p) == 1);
    assert(pixel == 16384 && s.timestamp_us == 133000);
    pixel = 9492;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 400001, &p) == 2 && pixel == 9492);
    pixel = 9496;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 400001, &p) == 2 && pixel == 9496);
    p.history_shift = 1; pixel = 9494;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 433001, &p) == 2 && pixel == 9494);
    p.enable = 0; pixel = 9480;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 466001, &p) == 0 && pixel == 9480 && !s.valid);
    p.enable = 1;
    assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 499001, &p) == 2 && pixel == 9480);
    for (i = 0; i < 3; ++i) {
        RAW_TEMPORAL_DENOISE_PARAMS bad = p;
        if (i == 0) bad.enable = 2;
        if (i == 1) bad.threshold_dn = 0;
        if (i == 2) bad.history_shift = 4;
        assert(raw_temporal_denoise_set_params(&bad) == -1);
        assert(raw_temporal_denoise_process(&s, &pixel, 2, 1, 1, 532001, &bad) == -1);
    }
    {
        uint16_t rows[2][3] = {{0, 16383, 65535}, {1000, 1004, 65535}};
        assert(raw_temporal_denoise_process(&s, &rows[0][0], 6, 2, 2, 600000, &p) == 2);
        assert(rows[0][0] == 0 && rows[0][1] == 16383 && rows[0][2] == 65535 && rows[1][2] == 65535);
        rows[1][0] = 1004;
        assert(raw_temporal_denoise_process(&s, &rows[0][0], 6, 2, 2, 633000, &p) == 1 && rows[1][0] == 1002);
        rows[0][1] = 16384;
        assert(raw_temporal_denoise_process(&s, &rows[0][0], 6, 2, 2, 666000, &p) == 1);
        assert(rows[0][1] == 16384 && rows[0][2] == 65535 && rows[1][2] == 65535);
        history = s.history_q8[0];
        assert(raw_temporal_denoise_process(&s, &rows[0][0], 3, 2, 2, 699000, &p) == RAW_TEMPORAL_ERROR_LAYOUT);
        assert(s.history_q8[0] == history && s.timestamp_us == 666000);
    }
    /* Corrected RAW16 keeps native DN units: no mask, clamp or right shift.
     * Exercise all weights at UINT16_MAX, including the largest Q8 sum. */
    for (i = 1; i <= 3; ++i) {
        RAW_TEMPORAL_DENOISE_STATE wide = {0};
        RAW_TEMPORAL_DENOISE_PARAMS q = {.enable = 1, .threshold_dn = 12, .history_shift = i};
        uint16_t value = 65535;
        assert(raw_temporal_denoise_process(&wide, &value, 2, 1, 1, 1000, &q) == 2);
        assert(value == 65535 && wide.history_q8[0] == 65535U * 256U);
        value = 65527;
        assert(raw_temporal_denoise_process(&wide, &value, 2, 1, 1, 34000, &q) == 1);
        assert(value == 65535U - (8U >> i));
        value = 20000;
        assert(raw_temporal_denoise_process(&wide, &value, 2, 1, 1, 67000, &q) == 1 && value == 20000);
        value = 20012;
        assert(raw_temporal_denoise_process(&wide, &value, 2, 1, 1, 100000, &q) == 1);
        assert(wide.history_q8[0] == 20000U * 256U + 12U * 256U / (1U << i));
        raw_temporal_denoise_destroy(&wide);
    }
    raw_temporal_denoise_destroy(&s); assert(!s.history_q8 && !s.valid && !s.capacity);
    raw_temporal_denoise_destroy(&s);
    test_spatial();
    test_selective_spatial();
    test_selective_separate_output();
    test_dual_speed();
    test_fused_reset();
#ifdef RAW_TEMPORAL_TEST_ALLOCATOR
    test_allocation_failure();
#endif
    puts("PASS: native RAW14/corrected UINT16 through 65535, all Q8 weights, threshold/reset/layout and padding");
    puts("PASS: spatial original-neighbor/Q8/edges, dual weights, motion entry/hold/hysteresis/reset and fused RAW bypass");
    return 0;
}
