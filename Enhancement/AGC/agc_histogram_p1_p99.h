#ifndef ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H
#define ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H

#include <stddef.h>
#include <stdint.h>

#define AGC_HISTOGRAM_P1_P99_STATUS_OK                0
#define AGC_HISTOGRAM_P1_P99_STATUS_FLAT_RANGE        1
#define AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT (-1)

typedef struct {
	uint32_t temporal_previous_percent;
	uint32_t percentile_low;  /* Integer percentile: 0..99, below high. */
	uint32_t percentile_high; /* Integer percentile: 1..100, above low. */
} AGC_HISTOGRAM_P1_P99_PARAMS;

typedef struct {
	uint16_t previous_p1;
	uint16_t previous_p99;
	AGC_HISTOGRAM_P1_P99_PARAMS previous_params;
	int temporal_valid;
} AGC_HISTOGRAM_P1_P99_STATE;

void agc_histogram_p1_p99_get_default_params(
	AGC_HISTOGRAM_P1_P99_PARAMS *params);
int agc_histogram_p1_p99_validate_params(
	const AGC_HISTOGRAM_P1_P99_PARAMS *params);
int agc_histogram_p1_p99_set_params(
	const AGC_HISTOGRAM_P1_P99_PARAMS *params);
void agc_histogram_p1_p99_get_params(
	AGC_HISTOGRAM_P1_P99_PARAMS *params);
void agc_histogram_p1_p99_state_reset(AGC_HISTOGRAM_P1_P99_STATE *state);
size_t agc_histogram_p1_p99_workspace_bins(void);
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
				   uint16_t *p99_result);

void agc_histogram_p1_p99_set_enabled(int enable);
int agc_histogram_p1_p99_is_enabled(void);
int agc_histogram_p1_p99_menu_run(void);

#endif /* ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H */
