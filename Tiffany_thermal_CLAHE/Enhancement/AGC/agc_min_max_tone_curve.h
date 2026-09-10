#ifndef ENHANCEMENT_AGC_MIN_MAX_TONE_CURVE_H
#define ENHANCEMENT_AGC_MIN_MAX_TONE_CURVE_H

#include <stdint.h>

#define AGC_MIN_MAX_TONE_CURVE_STATUS_OK                0
#define AGC_MIN_MAX_TONE_CURVE_STATUS_FLAT_RANGE        1
#define AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT (-1)

#define AGC_MIN_MAX_TONE_CURVE_LEFT_LUT_COUNT  65U
#define AGC_MIN_MAX_TONE_CURVE_RIGHT_LUT_COUNT 17U

typedef struct {
	uint16_t raw14_min;
	uint16_t raw14_max;
	uint16_t tone12_min;
	uint16_t tone12_max;
	uint16_t left[AGC_MIN_MAX_TONE_CURVE_LEFT_LUT_COUNT];
	uint16_t right[AGC_MIN_MAX_TONE_CURVE_RIGHT_LUT_COUNT];
} AGC_MIN_MAX_TONE_CURVE_LUT;

int agc_min_max_tone_curve_build_u14(
	const uint16_t *input,
	uint32_t input_stride_bytes,
	uint32_t width,
	uint32_t height,
	AGC_MIN_MAX_TONE_CURVE_LUT *lut);

void agc_min_max_tone_curve_set_enabled(int enable);
int agc_min_max_tone_curve_is_enabled(void);
int agc_min_max_tone_curve_menu_run(void);

#endif /* ENHANCEMENT_AGC_MIN_MAX_TONE_CURVE_H */
