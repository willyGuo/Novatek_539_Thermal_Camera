#include "agc_min_max_tone_curve.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agc_mode.h"

/*
 * Min/Max Tone Curve AGC core for 14-bit thermal RAW stored in UINT16.
 *
 * The algorithm follows the original colleague implementation:
 *   1. Find the full-frame RAW14 minimum and maximum.
 *   2. Convert them to the ISP 12-bit tone-curve domain with >> 2.
 *   3. Build the 65-point left and 17-point right manual tone LUTs.
 *
 * Mapping HD_VIDEO_FRAME memory and applying IQT_ITEM_TONE_PARAM remain the
 * responsibility of the liveview pipeline owner.
 */

#define AGC_RAW14_MAX       16383U
#define AGC_TONE12_MAX       4095U
#define AGC_TONE_LEFT_STEP     32U
#define AGC_TONE_RIGHT_START 2048U
#define AGC_TONE_RIGHT_STEP   128U
#define MENU_OPTION_REFRESH    (-2)

static int agc_min_max_tone_curve_read_option(void)
{
	char buf[32];
	char *end;
	unsigned long value;

	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == NULL) {
		return -1;
	}

	if (buf[0] == 'q' || buf[0] == 'Q') {
		return -1;
	}

	value = strtoul(buf, &end, 0);
	if (end == buf) {
		if (buf[0] != '\n' && buf[0] != '\r') {
			printf("Invalid option\n");
		}
		return MENU_OPTION_REFRESH;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0' || value > INT_MAX) {
		printf("Invalid option\n");
		return MENU_OPTION_REFRESH;
	}

	return (int)value;
}

static uint16_t agc_min_max_tone_curve_map_value(uint32_t x,
						 uint32_t min_value,
						 uint32_t max_value)
{
	uint32_t range = max_value - min_value;

	if (x <= min_value) {
		return 0U;
	}
	if (x >= max_value) {
		return AGC_TONE12_MAX;
	}
	return (uint16_t)((x - min_value) * AGC_TONE12_MAX / range);
}

int agc_min_max_tone_curve_build_u14(
	const uint16_t *input,
	uint32_t input_stride_bytes,
	uint32_t width,
	uint32_t height,
	AGC_MIN_MAX_TONE_CURVE_LUT *lut)
{
	uint16_t min_value = AGC_RAW14_MAX;
	uint16_t max_value = 0U;
	uint32_t min_tone;
	uint32_t max_tone;
	uint32_t x;
	uint32_t y;
	uint32_t i;

	if (input == NULL || lut == NULL || width == 0U || height == 0U ||
	    width > UINT32_MAX / sizeof(uint16_t) ||
	    input_stride_bytes < width * sizeof(uint16_t) ||
	    (input_stride_bytes & 1U) != 0U) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}

	memset(lut, 0, sizeof(*lut));
	for (y = 0; y < height; y++) {
		const uint16_t *input_row =
			(const uint16_t *)((const uint8_t *)input +
					     (size_t)y * input_stride_bytes);

		for (x = 0; x < width; x++) {
			uint16_t value = input_row[x] & AGC_RAW14_MAX;

			if (value < min_value) {
				min_value = value;
			}
			if (value > max_value) {
				max_value = value;
			}
		}
	}

	min_tone = min_value >> 2;
	max_tone = max_value >> 2;
	lut->raw14_min = min_value;
	lut->raw14_max = max_value;
	lut->tone12_min = (uint16_t)min_tone;
	lut->tone12_max = (uint16_t)max_tone;

	if (max_tone <= min_tone) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_FLAT_RANGE;
	}

	for (i = 0; i < AGC_MIN_MAX_TONE_CURVE_LEFT_LUT_COUNT; i++) {
		lut->left[i] = agc_min_max_tone_curve_map_value(
			i * AGC_TONE_LEFT_STEP, min_tone, max_tone);
	}
	for (i = 0; i < AGC_MIN_MAX_TONE_CURVE_RIGHT_LUT_COUNT; i++) {
		lut->right[i] = agc_min_max_tone_curve_map_value(
			AGC_TONE_RIGHT_START + i * AGC_TONE_RIGHT_STEP,
			min_tone, max_tone);
	}

	return AGC_MIN_MAX_TONE_CURVE_STATUS_OK;
}

void agc_min_max_tone_curve_set_enabled(int enable)
{
	if (enable) {
		agc_mode_set(AGC_MODE_MIN_MAX_TONE_CURVE_ISP);
	} else if (agc_mode_get() == AGC_MODE_MIN_MAX_TONE_CURVE_ISP) {
		agc_mode_set(AGC_MODE_OFF);
	}
}

int agc_min_max_tone_curve_is_enabled(void)
{
	return agc_mode_get() == AGC_MODE_MIN_MAX_TONE_CURVE_ISP;
}

int agc_min_max_tone_curve_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("Min/Max Tone Curve AGC (ISP) status\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("selected runtime AGC: %s\n",
		       agc_mode_name(agc_mode_get()));
		printf("mapping: RAW14 Min/Max -> ISP 12-bit manual Tone Curve\n");
		printf("pipeline: after RAW snapshot, before VideoProc\n");
		printf("Settings: CFG Setting -> AGC setting (mutually exclusive)\n");

		option = agc_min_max_tone_curve_read_option();
		switch (option) {
		case 0:
		case -1:
			running = 0;
			break;
		case MENU_OPTION_REFRESH:
			break;
		default:
			printf("Invalid option\n");
			break;
		}
	}

	return 0;
}
