#ifndef DIAGNOSTICS_ISP_RAW_TO_Y_STEP_DIAGNOSTIC_H
#define DIAGNOSTICS_ISP_RAW_TO_Y_STEP_DIAGNOSTIC_H

#include <stdint.h>

#define ISP_RAW_TO_Y_STEP_COUNT 64U
#define ISP_RAW_TO_Y_STEP_RAW12_MAX 4095U
#define ISP_RAW_TO_Y_STEP_RAW14_MAX 16383U
#define ISP_RAW_TO_Y_STEP_FINE_MIN 6000U
#define ISP_RAW_TO_Y_STEP_FINE_MAX 9000U

int isp_raw_to_y_step_diagnostic_get_next_run_id(uint32_t *run_id);
int isp_raw_to_y_step_diagnostic_generate(uint16_t *raw,
				    uint32_t raw_stride_bytes,
				    uint32_t width,
				    uint32_t height,
				    uint32_t min_raw_dn,
				    uint32_t max_raw_dn);
int isp_raw_to_y_step_diagnostic_save_and_analyze(
	uint32_t run_id,
	uint32_t min_raw_dn,
	uint32_t max_raw_dn,
	const uint16_t *raw,
	uint32_t raw_stride_bytes,
	const uint8_t *y_plane,
	uint32_t y_stride_bytes,
	uint32_t width,
	uint32_t height);
void isp_raw_to_y_step_diagnostic_print_last_result(void);

#endif /* DIAGNOSTICS_ISP_RAW_TO_Y_STEP_DIAGNOSTIC_H */
