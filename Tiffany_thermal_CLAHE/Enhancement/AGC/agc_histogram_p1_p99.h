#ifndef ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H
#define ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H

#include <stddef.h>
#include <stdint.h>

#define AGC_HISTOGRAM_P1_P99_STATUS_OK                0
#define AGC_HISTOGRAM_P1_P99_STATUS_FLAT_RANGE        1
#define AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT (-1)

size_t agc_histogram_p1_p99_workspace_bins(void);
int agc_histogram_p1_p99_apply_u14(const uint16_t *input,
				   uint32_t input_stride_bytes,
				   uint16_t *output,
				   uint32_t output_stride_bytes,
				   uint32_t width,
				   uint32_t height,
				   uint32_t *histogram_workspace,
				   size_t histogram_bins,
				   uint16_t *p1_result,
				   uint16_t *p99_result);

void agc_histogram_p1_p99_set_enabled(int enable);
int agc_histogram_p1_p99_is_enabled(void);
int agc_histogram_p1_p99_menu_run(void);

#endif /* ENHANCEMENT_AGC_HISTOGRAM_P1_P99_H */
