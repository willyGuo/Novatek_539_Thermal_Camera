#include "Enhancement/RAW/raw_temporal_denoise.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    RAW_TEMPORAL_DENOISE_STATE s = {0};
    RAW_TEMPORAL_DENOISE_PARAMS p = {1, 12, 2}, got;
    uint16_t pixel = 9400;
    uint32_t history;
    unsigned i;
    raw_temporal_denoise_get_params(&got);
    assert(!got.enable && got.threshold_dn == 12 && got.history_shift == 2);
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
        RAW_TEMPORAL_DENOISE_PARAMS q = {1, 12, i};
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
    puts("PASS: native RAW14/corrected UINT16 through 65535, all Q8 weights, threshold/reset/layout and padding");
    return 0;
}
