#ifndef AGC_WEIGHTED_H
#define AGC_WEIGHTED_H
#include <stddef.h>
#include <stdint.h>

#define AGC_WEIGHTED_BINS 16384U
typedef struct {
    uint32_t clip_tenths;             /* 0 off; otherwise x pixels / unique DN */
    uint32_t redistribute;
    uint32_t bright_weight_percent;   /* 100, 105, 110..200 */
    uint32_t bright_percentile_tenths;
    uint32_t gain_limit_tenths;        /* 0 off; 1..40 in 8-bit Gray / RAW DN */
    uint32_t temporal_previous_percent; /* 0 off; 1..99 previous LUT weight */
} AGC_WEIGHTED_PARAMS;
typedef struct {
    uint32_t histogram[AGC_WEIGHTED_BINS];
    uint32_t work[AGC_WEIGHTED_BINS];
    uint8_t lut[AGC_WEIGHTED_BINS];
    uint16_t previous_lut_tenths[AGC_WEIGHTED_BINS];
    AGC_WEIGHTED_PARAMS last_params;
    uint32_t last_width, last_height;
    int temporal_valid;
} AGC_WEIGHTED_WORKSPACE;

void agc_weighted_get_params(AGC_WEIGHTED_PARAMS *params);
int agc_weighted_validate_params(const AGC_WEIGHTED_PARAMS *params);
int agc_weighted_set_params(const AGC_WEIGHTED_PARAMS *params);
int agc_weighted_is_enabled(void);
int agc_weighted_menu_run(void);
void agc_weighted_state_reset(AGC_WEIGHTED_WORKSPACE *workspace);
/* Toolkit native-DN -> rounded uint8 LUT, then expand Gray to RAW14 for ISP.
 * No allocation or automatic ACI/THL/THH. Workspace must start zeroed; temporal
 * smoothing blends LUTs before Gain Limit, never prior-frame image pixels. */
int agc_weighted_apply_u14(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *workspace, const AGC_WEIGHTED_PARAMS *params);
#endif
