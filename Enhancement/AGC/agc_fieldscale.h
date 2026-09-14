#ifndef ENHANCEMENT_AGC_FIELDSCALE_H
#define ENHANCEMENT_AGC_FIELDSCALE_H

#include <stddef.h>
#include <stdint.h>

#define AGC_FIELDSCALE_GRID_MAX 32U
#define AGC_FIELDSCALE_GRID_COUNT (AGC_FIELDSCALE_GRID_MAX * AGC_FIELDSCALE_GRID_MAX)

typedef struct {
    uint32_t grid_size;
    uint32_t les_distance;
    uint32_t les_threshold;
    uint32_t message_distance;
    uint32_t message_iterations;
    uint32_t temporal_previous_percent;
} AGC_FIELDSCALE_PARAMS;

void agc_fieldscale_get_default_params(AGC_FIELDSCALE_PARAMS *params);

int agc_fieldscale_validate_params(const AGC_FIELDSCALE_PARAMS *params);
int agc_fieldscale_set_params(const AGC_FIELDSCALE_PARAMS *params);
void agc_fieldscale_get_params(AGC_FIELDSCALE_PARAMS *params);

#define AGC_FIELDSCALE_STATUS_OK                0
#define AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT (-1)

typedef struct {
	uint32_t previous_min_q8[AGC_FIELDSCALE_GRID_COUNT];
	uint32_t previous_max_q8[AGC_FIELDSCALE_GRID_COUNT];
	int temporal_valid;
	AGC_FIELDSCALE_PARAMS previous_params;
} AGC_FIELDSCALE_STATE;

typedef struct {
	uint64_t map_cache_us;
	uint64_t core_us;
	uint64_t output_flush_us;
	uint64_t unmap_us;
	uint64_t total_us;
	uint64_t completed_at_us;
} AGC_FIELDSCALE_LATENCY_SAMPLE;

void agc_fieldscale_state_reset(AGC_FIELDSCALE_STATE *state);
size_t agc_fieldscale_horizontal_workspace_values(uint32_t width);
int agc_fieldscale_apply_u14(const uint16_t *input,
			     uint32_t input_stride_bytes,
			     uint16_t *output,
			     uint32_t output_stride_bytes,
			     uint32_t width,
			     uint32_t height,
			     AGC_FIELDSCALE_STATE *state,
			     uint32_t *horizontal_workspace,
			     size_t horizontal_workspace_values);

void agc_fieldscale_set_enabled(int enable);
int agc_fieldscale_is_enabled(void);
void agc_fieldscale_latency_request(void);
int agc_fieldscale_latency_is_active(void);
void agc_fieldscale_latency_record(
	const AGC_FIELDSCALE_LATENCY_SAMPLE *sample);
int agc_fieldscale_menu_run(void);

#endif /* ENHANCEMENT_AGC_FIELDSCALE_H */
