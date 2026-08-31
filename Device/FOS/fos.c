#include "fos.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_OPTION_REFRESH (-2)
#define FOS_VALUE_MIN       0U
#define FOS_VALUE_MAX       63U
#define FOS_SEARCH_LIMIT    7U
#define FOS_RAW14_MASK      0x3FFFU

typedef struct {
	UINT16 target_raw14;
	UINT32 raw_width;
	UINT32 raw_height;
	UINT32 last_mean_raw14;
	UINT8 last_fos;
	unsigned int request_count;
	int last_result;
	int last_result_valid;
	FOS_CAPTURE_RAW_CB capture_raw;
	FOS_CALIBRATION_STATE_CB calibration_state;
} FOS_CONTEXT;

static FOS_CONTEXT g_fos = {
	.target_raw14 = FOS_RAW14_TARGET_DEFAULT,
	.last_result = -1,
};

static int fos_sensor_read_value(UINT8 *fos_value)
{
	if (fos_value == NULL) {
		return -1;
	}

	/*
	 * TODO: Connect this function after the RTD6122C SPI read protocol has
	 * been verified. FOS occupies FOCC bits F84..F79 across Byte10/Byte9,
	 * so the implementation must read the complete configuration value and
	 * extract FOS<5:0> without changing the neighboring fields.
	 *
	 * Example integration point:
	 *   1. Read the current FOCC configuration through Driver/SPI.
	 *   2. Extract FOS<5:0> into *fos_value.
	 *   3. Return 0 only after the received value has been validated.
	 */
	(void)fos_value;
	printf("FOS SPI read is not connected yet\n");
	return -1;
}

static int fos_sensor_write_value(UINT8 fos_value)
{
	if (fos_value > FOS_VALUE_MAX) {
		return -1;
	}

	/*
	 * TODO: Connect this function together with fos_sensor_read_value().
	 * Perform a read-modify-write of FOCC FOS<5:0> through Driver/SPI and
	 * preserve every non-FOS bit. Read the value back before returning 0.
	 */
	(void)fos_value;
	printf("FOS SPI write is not connected yet\n");
	return -1;
}

static UINT32 fos_abs_error(UINT32 mean_raw14, UINT32 target_raw14)
{
	return mean_raw14 >= target_raw14 ?
		mean_raw14 - target_raw14 : target_raw14 - mean_raw14;
}

static int fos_capture_mean(UINT16 *raw_buffer, UINT32 pixel_count,
			    UINT32 *mean_raw14)
{
	UINT64 sum = 0;
	UINT32 i;
	UINT32 raw_size;

	if (raw_buffer == NULL || mean_raw14 == NULL || pixel_count == 0U ||
	    g_fos.capture_raw == NULL ||
	    pixel_count > UINT_MAX / (UINT32)sizeof(*raw_buffer)) {
		return -1;
	}
	raw_size = pixel_count * (UINT32)sizeof(*raw_buffer);
	if (g_fos.capture_raw((UINTPTR)raw_buffer, raw_size) != 0) {
		printf("FOS RAW14 capture failed\n");
		return -1;
	}

	for (i = 0; i < pixel_count; i++) {
		sum += raw_buffer[i] & FOS_RAW14_MASK;
	}
	*mean_raw14 = (UINT32)(sum / pixel_count);
	return 0;
}

static int fos_read_option(void)
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

static int fos_confirm_adjustment(void)
{
	char buf[32];
	int ch;

	printf("Press Enter to start one-shot FOS adjustment; "
	       "any other key cancels\n");
	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == NULL) {
		printf("FOS adjustment canceled\n");
		return 0;
	}
	if (buf[0] == '\n' ||
	    (buf[0] == '\r' && (buf[1] == '\n' || buf[1] == '\0'))) {
		return 1;
	}

	/* Drain an overlong cancel input so it cannot become the next menu input. */
	if (strchr(buf, '\n') == NULL) {
		while ((ch = getchar()) != '\n' && ch != EOF) {
			/* discard */
		}
	}
	printf("FOS adjustment canceled\n");
	return 0;
}

void fos_bind_capture_callback(FOS_CAPTURE_RAW_CB capture_raw,
			       UINT32 raw_width, UINT32 raw_height)
{
	g_fos.capture_raw = capture_raw;
	g_fos.raw_width = raw_width;
	g_fos.raw_height = raw_height;
}

void fos_bind_calibration_state_callback(
	FOS_CALIBRATION_STATE_CB calibration_state)
{
	g_fos.calibration_state = calibration_state;
}

UINT16 fos_get_target_raw14(void)
{
	return g_fos.target_raw14;
}

int fos_adjust_once(void)
{
	UINT16 *raw_buffer = NULL;
	UINT32 pixel_count;
	UINT32 low = FOS_VALUE_MIN;
	UINT32 high = FOS_VALUE_MAX;
	UINT32 iteration;
	UINT32 best_error = UINT_MAX;
	UINT32 best_mean = 0;
	UINT8 best_fos = 0;
	UINT8 original_fos = 0;
	int sensor_value_changed = 0;
	int calibration_active = 0;
	int ret = -1;

	g_fos.request_count++;
	g_fos.last_result = -1;
	g_fos.last_result_valid = 0;

	printf("FOS one-shot adjustment requested: target RAW14 mean=%u\n",
	       (unsigned int)g_fos.target_raw14);
	if (g_fos.capture_raw == NULL || g_fos.raw_width == 0U ||
	    g_fos.raw_height == 0U ||
	    g_fos.raw_width > UINT_MAX / g_fos.raw_height) {
		printf("FOS RAW capture is unavailable; start Liveview first\n");
		goto exit;
	}
	pixel_count = g_fos.raw_width * g_fos.raw_height;
	if (pixel_count > UINT_MAX / (UINT32)sizeof(*raw_buffer)) {
		printf("FOS RAW buffer size is invalid\n");
		goto exit;
	}
	raw_buffer = (UINT16 *)malloc(pixel_count * sizeof(*raw_buffer));
	if (raw_buffer == NULL) {
		printf("FOS RAW buffer allocation failed\n");
		goto exit;
	}

	if (g_fos.calibration_state != NULL) {
		g_fos.calibration_state("FOS", 1);
		calibration_active = 1;
	}

	/* SPI read is intentionally left as a stub until board verification. */
	if (fos_sensor_read_value(&original_fos) != 0) {
		printf("FOS adjustment unavailable until SPI read is implemented\n");
		goto exit;
	}
	printf("FOS original value=%u\n", (unsigned int)original_fos);

	for (iteration = 0;
	     iteration < FOS_SEARCH_LIMIT && low <= high;
	     iteration++) {
		UINT32 mid = low + (high - low) / 2U;
		UINT32 mean_raw14;
		UINT32 error;

		if (fos_sensor_write_value((UINT8)mid) != 0) {
			printf("FOS write %u failed\n", (unsigned int)mid);
			goto restore;
		}
		sensor_value_changed = 1;
		if (fos_capture_mean(raw_buffer, pixel_count, &mean_raw14) != 0) {
			goto restore;
		}
		error = fos_abs_error(mean_raw14, g_fos.target_raw14);
		printf("FOS search %u/%u: value=%u, RAW14 mean=%u, error=%u\n",
		       (unsigned int)(iteration + 1U),
		       (unsigned int)FOS_SEARCH_LIMIT,
		       (unsigned int)mid, (unsigned int)mean_raw14,
		       (unsigned int)error);
		if (error < best_error) {
			best_error = error;
			best_mean = mean_raw14;
			best_fos = (UINT8)mid;
		}
		if (mean_raw14 == g_fos.target_raw14) {
			break;
		}
		if (mean_raw14 < g_fos.target_raw14) {
			low = mid + 1U;
		} else if (mid == 0U) {
			break;
		} else {
			high = mid - 1U;
		}
	}

	if (best_error == UINT_MAX ||
	    fos_sensor_write_value(best_fos) != 0) {
		printf("FOS best value apply failed\n");
		goto restore;
	}
	if (fos_capture_mean(raw_buffer, pixel_count, &best_mean) != 0) {
		goto restore;
	}
	best_error = fos_abs_error(best_mean, g_fos.target_raw14);
	g_fos.last_fos = best_fos;
	g_fos.last_mean_raw14 = best_mean;
	g_fos.last_result_valid = 1;
	printf("FOS adjustment complete: value=%u, RAW14 mean=%u, error=%u\n",
	       (unsigned int)best_fos, (unsigned int)best_mean,
	       (unsigned int)best_error);
	ret = 0;
	goto exit;

restore:
	if (sensor_value_changed &&
	    fos_sensor_write_value(original_fos) != 0) {
		printf("FOS restore original value %u failed\n",
		       (unsigned int)original_fos);
	} else if (sensor_value_changed) {
		printf("FOS original value %u restored\n",
		       (unsigned int)original_fos);
	}

exit:
	if (calibration_active) {
		g_fos.calibration_state("FOS", 0);
	}
	free(raw_buffer);
	g_fos.last_result = ret;
	return ret;
}

int fos_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("FOS menu\n");
		printf(" 1. Run one-shot FOS adjustment\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("mode: menu-triggered only, target RAW14 mean=%u\n",
		       (unsigned int)g_fos.target_raw14);
		printf("implementation: SPI pending, requests=%u, last=%s\n",
		       g_fos.request_count,
		       g_fos.request_count == 0U ? "not run" :
		       (g_fos.last_result == 0 ? "PASS" : "FAIL"));
		if (g_fos.last_result_valid) {
			printf("last result: FOS=%u, RAW14 mean=%u\n",
			       (unsigned int)g_fos.last_fos,
			       (unsigned int)g_fos.last_mean_raw14);
		}

		option = fos_read_option();
		switch (option) {
		case 1:
			if (fos_confirm_adjustment()) {
				(void)fos_adjust_once();
			}
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
