#include "agc_histogram_p1_p99.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agc_mode.h"
#if defined(__LINUX)
#include <pthread.h>
#else
#include <FreeRTOS_POSIX.h>
#include <FreeRTOS_POSIX/pthread.h>
#endif

/*
 * Histogram P1/P99 AGC core for 14-bit thermal RAW stored in UINT16.
 *
 * This file intentionally contains only the CPU algorithm. The liveview
 * owner is responsible for mapping HD_VIDEO_FRAME memory, synchronizing the
 * cache, and providing buffers whose lifetime covers this call.
 *
 * Input and output may be the same buffer for exact in-place processing, or
 * they may be separate non-overlapping buffers. Padding bytes at the end of a
 * row are not modified.
 */

#define AGC_RAW_BITS                 14U
#define AGC_RAW_MAX                  ((1U << AGC_RAW_BITS) - 1U)
#define AGC_HISTOGRAM_BIN_COUNT      (1U << AGC_RAW_BITS)
#define AGC_PERCENT_SCALE            100U
#define AGC_PERCENTILE_LOW           1U
#define AGC_PERCENTILE_HIGH          99U
#define MENU_OPTION_REFRESH          (-2)

#define AGC_HISTOGRAM_P1_P99_DEFAULT_PARAMS {90U}
static const AGC_HISTOGRAM_P1_P99_PARAMS g_histogram_defaults =
	AGC_HISTOGRAM_P1_P99_DEFAULT_PARAMS;
static AGC_HISTOGRAM_P1_P99_PARAMS g_histogram_params =
	AGC_HISTOGRAM_P1_P99_DEFAULT_PARAMS;
static pthread_mutex_t g_histogram_params_lock = PTHREAD_MUTEX_INITIALIZER;

void agc_histogram_p1_p99_get_default_params(
	AGC_HISTOGRAM_P1_P99_PARAMS *params)
{
	if (params != NULL) {
		*params = g_histogram_defaults;
	}
}

int agc_histogram_p1_p99_validate_params(
	const AGC_HISTOGRAM_P1_P99_PARAMS *params)
{
	if (params == NULL || params->temporal_previous_percent > 99U) {
		return -1;
	}
	return 0;
}

int agc_histogram_p1_p99_set_params(
	const AGC_HISTOGRAM_P1_P99_PARAMS *params)
{
	if (agc_histogram_p1_p99_validate_params(params) != 0) {
		return -1;
	}
	pthread_mutex_lock(&g_histogram_params_lock);
	g_histogram_params = *params;
	pthread_mutex_unlock(&g_histogram_params_lock);
	return 0;
}

void agc_histogram_p1_p99_get_params(
	AGC_HISTOGRAM_P1_P99_PARAMS *params)
{
	if (params == NULL) {
		return;
	}
	pthread_mutex_lock(&g_histogram_params_lock);
	*params = g_histogram_params;
	pthread_mutex_unlock(&g_histogram_params_lock);
}

void agc_histogram_p1_p99_state_reset(AGC_HISTOGRAM_P1_P99_STATE *state)
{
	if (state != NULL) {
		memset(state, 0, sizeof(*state));
	}
}

static int agc_histogram_p1_p99_read_option(void)
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

size_t agc_histogram_p1_p99_workspace_bins(void)
{
	return AGC_HISTOGRAM_BIN_COUNT;
}

/*
 * Return values:
 *   0  AGC applied successfully.
 *   1  P99 <= P1; the RAW14 input was copied without stretching.
 *  -1  Invalid pointer, size, stride, or workspace.
 */
int agc_histogram_p1_p99_apply_u14(const uint16_t *input,
				   uint32_t input_stride_bytes,
				   uint16_t *output,
				   uint32_t output_stride_bytes,
				   uint32_t width,
				   uint32_t height,
				   uint32_t *histogram_workspace,
				   size_t histogram_bins,
				   AGC_HISTOGRAM_P1_P99_STATE *state,
				   uint16_t *p1_result,
				   uint16_t *p99_result)
{
	AGC_HISTOGRAM_P1_P99_PARAMS params;
	uint64_t pixel_count;
	uint64_t low_target;
	uint64_t high_target;
	uint64_t cumulative;
	uint16_t p1_value = 0;
	uint16_t p99_value = AGC_RAW_MAX;
	uint32_t x;
	uint32_t y;
	uint32_t bin;
	uint32_t range;

	agc_histogram_p1_p99_get_params(&params);
	if (input == NULL || output == NULL || histogram_workspace == NULL ||
	    state == NULL ||
	    width == 0U || height == 0U ||
	    histogram_bins < AGC_HISTOGRAM_BIN_COUNT ||
	    input_stride_bytes < width * sizeof(uint16_t) ||
	    output_stride_bytes < width * sizeof(uint16_t) ||
	    (input_stride_bytes & 1U) != 0U ||
	    (output_stride_bytes & 1U) != 0U) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	pixel_count = (uint64_t)width * height;
	if (pixel_count > UINT32_MAX) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	memset(histogram_workspace, 0,
	       AGC_HISTOGRAM_BIN_COUNT * sizeof(histogram_workspace[0]));

	for (y = 0; y < height; y++) {
		const uint16_t *input_row =
			(const uint16_t *)((const uint8_t *)input +
					     (size_t)y * input_stride_bytes);

		for (x = 0; x < width; x++) {
			bin = input_row[x] & AGC_RAW_MAX;
			histogram_workspace[bin]++;
		}
	}

	/* Match the original integer histogram definition: 1% from each tail. */
	low_target = pixel_count * AGC_PERCENTILE_LOW / AGC_PERCENT_SCALE;
	high_target = pixel_count *
		(AGC_PERCENT_SCALE - AGC_PERCENTILE_HIGH) /
		AGC_PERCENT_SCALE;
	if (low_target == 0U) {
		low_target = 1U;
	}
	if (high_target == 0U) {
		high_target = 1U;
	}

	cumulative = 0U;
	for (bin = 0; bin < AGC_HISTOGRAM_BIN_COUNT; bin++) {
		cumulative += histogram_workspace[bin];
		if (cumulative >= low_target) {
			p1_value = (uint16_t)bin;
			break;
		}
	}

	cumulative = 0U;
	for (bin = AGC_HISTOGRAM_BIN_COUNT; bin > 0U; bin--) {
		uint32_t current_bin = bin - 1U;

		cumulative += histogram_workspace[current_bin];
		if (cumulative >= high_target) {
			p99_value = (uint16_t)current_bin;
			break;
		}
	}

	if (memcmp(&state->previous_params, &params, sizeof(params)) != 0) {
		state->temporal_valid = 0;
		state->previous_params = params;
	}
	if (state->temporal_valid) {
		p1_value = (uint16_t)(
			((uint32_t)p1_value *
			 (AGC_PERCENT_SCALE - params.temporal_previous_percent) +
			 (uint32_t)state->previous_p1 *
			 params.temporal_previous_percent +
			 AGC_PERCENT_SCALE / 2U) /
			 AGC_PERCENT_SCALE);
		p99_value = (uint16_t)(
			((uint32_t)p99_value *
			 (AGC_PERCENT_SCALE - params.temporal_previous_percent) +
			 (uint32_t)state->previous_p99 *
			 params.temporal_previous_percent +
			 AGC_PERCENT_SCALE / 2U) /
			 AGC_PERCENT_SCALE);
	}
	state->previous_p1 = p1_value;
	state->previous_p99 = p99_value;
	state->temporal_valid = 1;

	if (p1_result != NULL) {
		*p1_result = p1_value;
	}
	if (p99_result != NULL) {
		*p99_result = p99_value;
	}

	if (p99_value <= p1_value) {
		for (y = 0; y < height; y++) {
			const uint16_t *input_row =
				(const uint16_t *)((const uint8_t *)input +
					(size_t)y * input_stride_bytes);
			uint16_t *output_row =
				(uint16_t *)((uint8_t *)output +
					(size_t)y * output_stride_bytes);

			for (x = 0; x < width; x++) {
				output_row[x] = input_row[x] & AGC_RAW_MAX;
			}
		}
		return AGC_HISTOGRAM_P1_P99_STATUS_FLAT_RANGE;
	}

	range = (uint32_t)p99_value - p1_value;
	for (y = 0; y < height; y++) {
		const uint16_t *input_row =
			(const uint16_t *)((const uint8_t *)input +
					     (size_t)y * input_stride_bytes);
		uint16_t *output_row =
			(uint16_t *)((uint8_t *)output +
				     (size_t)y * output_stride_bytes);

		for (x = 0; x < width; x++) {
			uint32_t value = input_row[x] & AGC_RAW_MAX;

			if (value <= p1_value) {
				output_row[x] = 0U;
			} else if (value >= p99_value) {
				output_row[x] = AGC_RAW_MAX;
			} else {
				output_row[x] = (uint16_t)(
					(value - p1_value) * AGC_RAW_MAX / range);
			}
		}
	}

	return AGC_HISTOGRAM_P1_P99_STATUS_OK;
}

void agc_histogram_p1_p99_set_enabled(int enable)
{
	if (enable) {
		agc_mode_set(AGC_MODE_HISTOGRAM_P1_P99_CPU);
	} else if (agc_mode_get() == AGC_MODE_HISTOGRAM_P1_P99_CPU) {
		agc_mode_set(AGC_MODE_OFF);
	}
}

int agc_histogram_p1_p99_is_enabled(void)
{
	return agc_mode_get() == AGC_MODE_HISTOGRAM_P1_P99_CPU;
}

int agc_histogram_p1_p99_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("Histogram P1/P99 AGC (CPU) status\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("selected runtime AGC: %s\n",
		       agc_mode_name(agc_mode_get()));
		printf("mapping: P1..P99 -> RAW14 0..16383, CPU Linear Stretch\n");
		printf("pipeline: after RAW snapshot, before VideoProc\n");
		printf("Settings: CFG Setting -> AGC setting (mutually exclusive)\n");

		option = agc_histogram_p1_p99_read_option();
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
