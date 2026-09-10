/*
 * Pure-C embedded adaptation of Fieldscale:
 *   Fieldscale: Locality-Aware Field-based Adaptive Rescaling for
 *   Thermal Infrared Image, H. Gil, M.-H. Jeon, and A. Kim.
 *
 * The original OpenCV/C++ implementation is Copyright (c) 2025 nowroad
 * and distributed under the MIT License.  See FIELDSCALE_LICENSE.txt.
 * This adaptation keeps configurable min/max fields, extrema suppression,
 * message passing, bilinear field expansion, and video temporal smoothing.
 * Gamma and CLAHE are intentionally omitted for the firmware pipeline.
 */

#include "agc_fieldscale.h"

#include <inttypes.h>
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

static pthread_mutex_t g_fieldscale_params_lock = PTHREAD_MUTEX_INITIALIZER;
#define FIELDSCALE_DEFAULT_PARAMS {8U, 2U, 400U, 1U, 7U, 90U}
static const AGC_FIELDSCALE_PARAMS g_fieldscale_defaults = FIELDSCALE_DEFAULT_PARAMS;
static AGC_FIELDSCALE_PARAMS g_fieldscale_params = FIELDSCALE_DEFAULT_PARAMS;

void agc_fieldscale_get_default_params(AGC_FIELDSCALE_PARAMS *params)
{
    if (params != NULL) *params = g_fieldscale_defaults;
}

int agc_fieldscale_validate_params(const AGC_FIELDSCALE_PARAMS *p)
{
    if (p == NULL || (p->grid_size != 2U && p->grid_size != 4U &&
        p->grid_size != 8U && p->grid_size != 16U && p->grid_size != 32U) ||
        p->les_distance < 1U || p->les_distance > 31U ||
        p->les_threshold > 16383U || p->message_distance < 1U ||
        p->message_distance > 31U || p->message_iterations > 32U ||
        p->temporal_previous_percent > 99U) {
        return -1;
    }
    return 0;
}

int agc_fieldscale_set_params(const AGC_FIELDSCALE_PARAMS *params)
{
    if (agc_fieldscale_validate_params(params) != 0) return -1;
    pthread_mutex_lock(&g_fieldscale_params_lock);
    g_fieldscale_params = *params;
    pthread_mutex_unlock(&g_fieldscale_params_lock);
    return 0;
}

void agc_fieldscale_get_params(AGC_FIELDSCALE_PARAMS *params)
{
    if (params == NULL) return;
    pthread_mutex_lock(&g_fieldscale_params_lock);
    *params = g_fieldscale_params;
    pthread_mutex_unlock(&g_fieldscale_params_lock);
}

#define FIELDSCALE_RAW14_MAX             16383U
#define FIELDSCALE_GRID_FRAC_BITS        8U
#define FIELDSCALE_GRID_FRAC_ONE         (1U << FIELDSCALE_GRID_FRAC_BITS)
#define FIELDSCALE_INTERP_ONE            65536U
#define FIELDSCALE_LATENCY_FRAME_COUNT   60U
#define MENU_OPTION_REFRESH              (-2)

typedef struct {
	uint64_t map_cache_us;
	uint64_t core_us;
	uint64_t output_flush_us;
	uint64_t unmap_us;
	uint64_t total_us;
	uint64_t max_total_us;
	uint64_t interval_us;
	uint64_t previous_completed_at_us;
	volatile uint32_t frames;
	uint32_t interval_count;
	volatile int active;
} AGC_FIELDSCALE_LATENCY_CONTEXT;

static AGC_FIELDSCALE_LATENCY_CONTEXT g_fieldscale_latency;

static int agc_fieldscale_read_option(void)
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

static uint32_t fieldscale_grid_index(uint32_t x, uint32_t y)
{
	return y * AGC_FIELDSCALE_GRID_MAX + x;
}

static void fieldscale_grid_min_max(const uint16_t *input,
				    uint32_t input_stride_bytes,
				    uint32_t width,
				    uint32_t height,
				    uint32_t *min_grid,
				    uint32_t *max_grid,
                                    const AGC_FIELDSCALE_PARAMS *params)
{
	uint32_t grid_x;
	uint32_t grid_y;

	for (grid_y = 0; grid_y < params->grid_size; grid_y++) {
		for (grid_x = 0; grid_x < params->grid_size; grid_x++) {
			uint32_t min_value = FIELDSCALE_RAW14_MAX;
			uint32_t max_value = 0U;
			uint32_t start_x = grid_x * width / params->grid_size;
			uint32_t start_y = grid_y * height / params->grid_size;
			uint32_t x;
			uint32_t y;

			for (y = start_y; y < (grid_y + 1U) * height / params->grid_size; y++) {
				const uint16_t *row = (const uint16_t *)(
					(const uint8_t *)input +
					(size_t)y * input_stride_bytes);
				for (x = start_x; x < (grid_x + 1U) * width / params->grid_size; x++) {
					uint32_t value = row[x] & FIELDSCALE_RAW14_MAX;
					if (value < min_value) {
						min_value = value;
					}
					if (value > max_value) {
						max_value = value;
					}
				}
			}

			min_grid[fieldscale_grid_index(grid_x, grid_y)] =
				min_value * FIELDSCALE_GRID_FRAC_ONE;
			max_grid[fieldscale_grid_index(grid_x, grid_y)] =
				max_value * FIELDSCALE_GRID_FRAC_ONE;
		}
	}
}

static void fieldscale_suppress_extrema(const uint32_t *grid,
					uint32_t *result,
					uint32_t threshold_q8,
					int suppress_maximum,
                                        const AGC_FIELDSCALE_PARAMS *params)
{
	uint32_t x;
	uint32_t y;

	memcpy(result, grid,
	       AGC_FIELDSCALE_GRID_COUNT * sizeof(result[0]));
	for (y = 0; y < params->grid_size; y++) {
		for (x = 0; x < params->grid_size; x++) {
			uint64_t sum = 0U;
			uint32_t count = 0U;
			uint32_t neighbor_min = UINT32_MAX;
			uint32_t neighbor_max = 0U;
			int dx;
			int dy;
			uint32_t current;
			uint32_t mean;

			for (dy = -(int)params->les_distance;
			     dy <= (int)params->les_distance; dy++) {
				for (dx = -(int)params->les_distance;
				     dx <= (int)params->les_distance; dx++) {
					int nx = (int)x + dx;
					int ny = (int)y + dy;
					uint32_t value;

					if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 ||
					    nx >= (int)params->grid_size ||
					    ny >= (int)params->grid_size) {
						continue;
					}
					value = grid[fieldscale_grid_index(
						(uint32_t)nx, (uint32_t)ny)];
					sum += value;
					count++;
					if (value < neighbor_min) {
						neighbor_min = value;
					}
					if (value > neighbor_max) {
						neighbor_max = value;
					}
				}
			}

			if (count == 0U) {
				continue;
			}
			current = grid[fieldscale_grid_index(x, y)];
			mean = (uint32_t)(sum / count);
			if (suppress_maximum) {
				if (current >= neighbor_max && current > mean &&
				    current - mean > threshold_q8) {
					result[fieldscale_grid_index(x, y)] =
						mean + threshold_q8;
				}
			} else if (current <= neighbor_min && mean > current &&
				   mean - current > threshold_q8) {
				result[fieldscale_grid_index(x, y)] =
					mean - threshold_q8;
			}
		}
	}
}

static void fieldscale_message_pass(const uint32_t *grid,
				    uint32_t *result,
				    int increase,
                                    const AGC_FIELDSCALE_PARAMS *params)
{
	uint32_t x;
	uint32_t y;

	for (y = 0; y < params->grid_size; y++) {
		for (x = 0; x < params->grid_size; x++) {
			uint64_t sum = grid[fieldscale_grid_index(x, y)];
			uint32_t count = 1U;
			uint32_t current = (uint32_t)sum;
			uint32_t mean;
			int dx;
			int dy;

			for (dy = -(int)params->message_distance; dy <= (int)params->message_distance; dy++) {
				for (dx = -(int)params->message_distance; dx <= (int)params->message_distance; dx++) {
					int nx = (int)x + dx;
					int ny = (int)y + dy;
					if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 ||
					    nx >= (int)params->grid_size ||
					    ny >= (int)params->grid_size) {
						continue;
					}
					sum += grid[fieldscale_grid_index(
						(uint32_t)nx, (uint32_t)ny)];
					count++;
				}
			}
			mean = (uint32_t)(sum / count);
			result[fieldscale_grid_index(x, y)] = increase ?
				(mean > current ? mean : current) :
				(mean < current ? mean : current);
		}
	}
}

static uint32_t fieldscale_lerp_q16(uint32_t a, uint32_t b, uint32_t fraction)
{
	return (uint32_t)(((uint64_t)a * (FIELDSCALE_INTERP_ONE - fraction) +
		(uint64_t)b * fraction + FIELDSCALE_INTERP_ONE / 2U) /
		FIELDSCALE_INTERP_ONE);
}

static void fieldscale_interpolation_from_q16(int64_t source_q16,
					     uint32_t grid_size,
					     uint32_t *index0,
					     uint32_t *index1,
					     uint32_t *fraction)
{
	int64_t maximum_q16 = (int64_t)(grid_size - 1U) * 65536;

	if (source_q16 <= 0) {
		*index0 = 0U;
		*index1 = 0U;
		*fraction = 0U;
	} else if (source_q16 >= maximum_q16) {
		*index0 = grid_size - 1U;
		*index1 = grid_size - 1U;
		*fraction = 0U;
	} else {
		*index0 = (uint32_t)(source_q16 >> 16);
		*index1 = *index0 + 1U;
		*fraction = (uint32_t)(source_q16 & 0xFFFF);
	}
}

void agc_fieldscale_state_reset(AGC_FIELDSCALE_STATE *state)
{
	if (state != NULL) {
		memset(state, 0, sizeof(*state));
	}
}

int agc_fieldscale_apply_u14(const uint16_t *input,
			     uint32_t input_stride_bytes,
			     uint16_t *output,
			     uint32_t output_stride_bytes,
			     uint32_t width,
			     uint32_t height,
			     AGC_FIELDSCALE_STATE *state)
{
	AGC_FIELDSCALE_PARAMS frame_params;
	const AGC_FIELDSCALE_PARAMS *params = &frame_params;
	uint32_t min_grid[AGC_FIELDSCALE_GRID_COUNT] = {0};
	uint32_t max_grid[AGC_FIELDSCALE_GRID_COUNT] = {0};
	uint32_t scratch[AGC_FIELDSCALE_GRID_COUNT] = {0};
	uint32_t x;
	uint32_t y;
	uint32_t iteration;
	int64_t source_y_q16;
	int64_t source_x_start_q16;
	int64_t source_x_step_q16;
	int64_t source_y_step_q16;

	agc_fieldscale_get_params(&frame_params);
	if (input == NULL || output == NULL || state == NULL ||
	    width < params->grid_size ||
	    height < params->grid_size ||
	    input_stride_bytes < width * sizeof(uint16_t) ||
	    output_stride_bytes < width * sizeof(uint16_t) ||
	    (input_stride_bytes & 1U) != 0U ||
	    (output_stride_bytes & 1U) != 0U) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}

    if (memcmp(&state->previous_params, params, sizeof(*params)) != 0) {
        state->temporal_valid = 0;
        state->previous_params = *params;
    }
	fieldscale_grid_min_max(input, input_stride_bytes, width, height,
				min_grid, max_grid, params);
	fieldscale_suppress_extrema(max_grid, scratch,
		params->les_threshold * FIELDSCALE_GRID_FRAC_ONE, 1, params);
	memcpy(max_grid, scratch, sizeof(max_grid));
	fieldscale_suppress_extrema(max_grid, scratch,
		params->les_threshold * FIELDSCALE_GRID_FRAC_ONE, 0, params);
	memcpy(max_grid, scratch, sizeof(max_grid));

	for (iteration = 0; iteration < params->message_iterations;
	     iteration++) {
		fieldscale_message_pass(min_grid, scratch, 0, params);
		memcpy(min_grid, scratch, sizeof(min_grid));
		fieldscale_message_pass(max_grid, scratch, 1, params);
		memcpy(max_grid, scratch, sizeof(max_grid));
	}

	if (state->temporal_valid) {
		for (x = 0; x < AGC_FIELDSCALE_GRID_COUNT; x++) {
			min_grid[x] = (min_grid[x] * (100U - params->temporal_previous_percent) +
				state->previous_min_q8[x] *
				params->temporal_previous_percent +
				100U / 2U) /
				100U;
			max_grid[x] = (max_grid[x] * (100U - params->temporal_previous_percent) +
				state->previous_max_q8[x] *
				params->temporal_previous_percent +
				100U / 2U) /
				100U;
		}
	}
	memcpy(state->previous_min_q8, min_grid, sizeof(min_grid));
	memcpy(state->previous_max_q8, max_grid, sizeof(max_grid));
	state->temporal_valid = 1;

	/* OpenCV INTER_LINEAR pixel-center coordinates, advanced incrementally. */
	source_x_start_q16 =
		((int64_t)params->grid_size * 32768) / width - 32768;
	source_x_step_q16 =
		((int64_t)params->grid_size * 65536) / width;
	source_y_q16 =
		((int64_t)params->grid_size * 32768) / height - 32768;
	source_y_step_q16 =
		((int64_t)params->grid_size * 65536) / height;
	for (y = 0; y < height; y++) {
		const uint16_t *input_row = (const uint16_t *)(
			(const uint8_t *)input + (size_t)y * input_stride_bytes);
		uint16_t *output_row = (uint16_t *)(
			(uint8_t *)output + (size_t)y * output_stride_bytes);
		uint32_t y0;
		uint32_t y1;
		uint32_t fy;
		int64_t source_x_q16 = source_x_start_q16;

		fieldscale_interpolation_from_q16(source_y_q16,
			params->grid_size, &y0, &y1, &fy);
		for (x = 0; x < width; x++) {
			uint32_t x0;
			uint32_t x1;
			uint32_t fx;
			uint32_t min_top;
			uint32_t min_bottom;
			uint32_t max_top;
			uint32_t max_bottom;
			uint32_t min_field;
			uint32_t max_field;
			uint32_t low;
			uint32_t high;
			uint32_t value_q8;

			fieldscale_interpolation_from_q16(source_x_q16,
				params->grid_size, &x0, &x1, &fx);
			min_top = fieldscale_lerp_q16(
				min_grid[fieldscale_grid_index(x0, y0)],
				min_grid[fieldscale_grid_index(x1, y0)], fx);
			min_bottom = fieldscale_lerp_q16(
				min_grid[fieldscale_grid_index(x0, y1)],
				min_grid[fieldscale_grid_index(x1, y1)], fx);
			max_top = fieldscale_lerp_q16(
				max_grid[fieldscale_grid_index(x0, y0)],
				max_grid[fieldscale_grid_index(x1, y0)], fx);
			max_bottom = fieldscale_lerp_q16(
				max_grid[fieldscale_grid_index(x0, y1)],
				max_grid[fieldscale_grid_index(x1, y1)], fx);
			min_field = fieldscale_lerp_q16(min_top, min_bottom, fy);
			max_field = fieldscale_lerp_q16(max_top, max_bottom, fy);
			low = min_field < max_field ? min_field : max_field;
			high = min_field < max_field ? max_field : min_field;
			value_q8 = (input_row[x] & FIELDSCALE_RAW14_MAX) *
				FIELDSCALE_GRID_FRAC_ONE;

			if (value_q8 <= low || high <= low) {
				output_row[x] = 0U;
			} else if (value_q8 >= high) {
				output_row[x] = FIELDSCALE_RAW14_MAX;
			} else {
				output_row[x] = (uint16_t)(
					(uint64_t)(value_q8 - low) *
					FIELDSCALE_RAW14_MAX / (high - low));
			}
			source_x_q16 += source_x_step_q16;
		}
		source_y_q16 += source_y_step_q16;
	}
	return AGC_FIELDSCALE_STATUS_OK;
}

void agc_fieldscale_set_enabled(int enable)
{
	if (enable) {
		agc_mode_set(AGC_MODE_FIELDSCALE_CPU);
	} else if (agc_mode_get() == AGC_MODE_FIELDSCALE_CPU) {
		agc_mode_set(AGC_MODE_OFF);
	}
}

int agc_fieldscale_is_enabled(void)
{
	return agc_mode_get() == AGC_MODE_FIELDSCALE_CPU;
}

void agc_fieldscale_latency_request(void)
{
	memset(&g_fieldscale_latency, 0, sizeof(g_fieldscale_latency));
	g_fieldscale_latency.active = 1;
}

int agc_fieldscale_latency_is_active(void)
{
	return g_fieldscale_latency.active;
}

void agc_fieldscale_latency_record(
	const AGC_FIELDSCALE_LATENCY_SAMPLE *sample)
{
	double frames;
	double fps = 0.0;
	uint64_t measured_us;
	uint64_t other_us;

	if (sample == NULL || !g_fieldscale_latency.active) {
		return;
	}

	g_fieldscale_latency.map_cache_us += sample->map_cache_us;
	g_fieldscale_latency.core_us += sample->core_us;
	g_fieldscale_latency.output_flush_us += sample->output_flush_us;
	g_fieldscale_latency.unmap_us += sample->unmap_us;
	g_fieldscale_latency.total_us += sample->total_us;
	if (sample->total_us > g_fieldscale_latency.max_total_us) {
		g_fieldscale_latency.max_total_us = sample->total_us;
	}
	if (g_fieldscale_latency.previous_completed_at_us != 0U &&
	    sample->completed_at_us >=
		g_fieldscale_latency.previous_completed_at_us) {
		g_fieldscale_latency.interval_us += sample->completed_at_us -
			g_fieldscale_latency.previous_completed_at_us;
		g_fieldscale_latency.interval_count++;
	}
	g_fieldscale_latency.previous_completed_at_us = sample->completed_at_us;
	g_fieldscale_latency.frames++;

	if (g_fieldscale_latency.frames < FIELDSCALE_LATENCY_FRAME_COUNT) {
		return;
	}

	/* Stop first so a new menu request cannot append to this report. */
	g_fieldscale_latency.active = 0;
	frames = (double)g_fieldscale_latency.frames;
	if (g_fieldscale_latency.interval_us != 0U) {
		fps = (double)g_fieldscale_latency.interval_count * 1000000.0 /
			(double)g_fieldscale_latency.interval_us;
	}
	measured_us = g_fieldscale_latency.map_cache_us +
		g_fieldscale_latency.core_us +
		g_fieldscale_latency.output_flush_us +
		g_fieldscale_latency.unmap_us;
	other_us = g_fieldscale_latency.total_us > measured_us ?
		g_fieldscale_latency.total_us - measured_us : 0U;

	printf("Fieldscale AGC latency (%" PRIu32 " frames): "
	       "map/cache=%.3f ms, CPU core=%.3f ms, "
	       "output flush=%.3f ms, unmap=%.3f ms, other=%.3f ms, "
	       "total=%.3f ms, max=%.3f ms, pipeline=%.2f FPS\n",
	       g_fieldscale_latency.frames,
	       (double)g_fieldscale_latency.map_cache_us / frames / 1000.0,
	       (double)g_fieldscale_latency.core_us / frames / 1000.0,
	       (double)g_fieldscale_latency.output_flush_us / frames / 1000.0,
	       (double)g_fieldscale_latency.unmap_us / frames / 1000.0,
	       (double)other_us / frames / 1000.0,
	       (double)g_fieldscale_latency.total_us / frames / 1000.0,
	       (double)g_fieldscale_latency.max_total_us / 1000.0,
	       fps);
}

int agc_fieldscale_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("Fieldscale Local Adaptive AGC (CPU)\n");
		printf(" 1. Run latency test (next 60 Fieldscale frames)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("selected runtime AGC: %s\n",
		       agc_mode_name(agc_mode_get()));
        {
            AGC_FIELDSCALE_PARAMS p;
            agc_fieldscale_get_params(&p);
            printf("core: %ux%u, LES distance %u, threshold %u RAW14 DN\n",
                   p.grid_size, p.grid_size, p.les_distance, p.les_threshold);
            printf("message distance %u, passes %u; temporal previous %u%%\n",
                   p.message_distance, p.message_iterations, p.temporal_previous_percent);
        }
		printf("output: local stretch -> RAW14 0..16383\n");
		printf("Gamma/CLAHE: not included in this firmware stage\n");
		printf("latency test: %s (%" PRIu32 "/%u frames)\n",
		       g_fieldscale_latency.active ? "collecting" : "idle",
		       g_fieldscale_latency.frames,
		       FIELDSCALE_LATENCY_FRAME_COUNT);
		printf("Settings: CFG Setting -> AGC setting (mutually exclusive)\n");

		option = agc_fieldscale_read_option();
		switch (option) {
		case 1:
			if (agc_fieldscale_latency_is_active()) {
				printf("Fieldscale AGC latency test is already "
				       "collecting (%" PRIu32 "/%u frames)\n",
				       g_fieldscale_latency.frames,
				       FIELDSCALE_LATENCY_FRAME_COUNT);
				break;
			}
			if (!agc_fieldscale_is_enabled()) {
				if (agc_mode_request(AGC_MODE_FIELDSCALE_CPU) != 0) {
                    printf("Select Fieldscale in CFG and restart Liveview before testing\n");
                    break;
                }
				printf("Fieldscale AGC selected for runtime latency test "
				       "(CFG is unchanged)\n");
			}
			agc_fieldscale_latency_request();
			printf("Fieldscale AGC latency test started; "
			       "one report will be printed after 60 frames\n");
			break;
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
