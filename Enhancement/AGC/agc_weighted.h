#ifndef AGC_WEIGHTED_H
#define AGC_WEIGHTED_H
#include <stddef.h>
#include <stdint.h>

#define AGC_WEIGHTED_BINS 16384U
#define AGC_WEIGHTED_FAST_LOCAL 4U
#define AGC_WEIGHTED_LOCAL_MEAN 5U
typedef struct {
    uint32_t clip_tenths;             /* 0 off; otherwise x pixels / unique DN */
    uint32_t redistribute;
    double bright_weight;            /* finite >= 1; no fixed upper limit */
    uint32_t bright_percentile_tenths;
    uint32_t gain_limit_tenths;        /* 0 off; 1..40 in 8-bit Gray / RAW DN */
    uint32_t temporal_previous_percent; /* 0 off; 1..99 previous weight for LUT/gain compensation */
    uint32_t simple_mode;              /* 0 Off, 1 Brightness, 2 Mild, 3 Strong, 4 Fast Local, 5 Local Mean */
} AGC_WEIGHTED_PARAMS;
#define AGC_WEIGHTED_DEFAULT_PARAMS {20, 1, 1.0, 700, 20, 90, 0}
typedef struct {
    uint32_t histogram[AGC_WEIGHTED_BINS];
    uint32_t scratch[AGC_WEIGHTED_BINS];
    uint16_t lut[AGC_WEIGHTED_BINS]; /* Native output DN: 0..16383. */
    uint32_t previous_lut_q8[AGC_WEIGHTED_BINS];
    AGC_WEIGHTED_PARAMS last_params;
    uint32_t last_width, last_height;
    int temporal_valid;
    uint32_t gain_offset_q8; /* Mean-brightness compensation, RAW14 DN in Q8. */
    int gain_offset_valid;
    float *local_buffer; /* Lazy 4-plane guided-filter scratch; no frame history. */
    size_t local_capacity_pixels;
} AGC_WEIGHTED_WORKSPACE;

void agc_weighted_default_params(AGC_WEIGHTED_PARAMS *params);
void agc_weighted_get_params(AGC_WEIGHTED_PARAMS *params);
int agc_weighted_validate_params(const AGC_WEIGHTED_PARAMS *params);
int agc_weighted_set_params(const AGC_WEIGHTED_PARAMS *params);
int agc_weighted_is_enabled(void);
int agc_weighted_menu_run(void);
void agc_weighted_state_reset(AGC_WEIGHTED_WORKSPACE *workspace);
/* Free internal scratch after the RAW worker has stopped, before freeing ws. */
void agc_weighted_workspace_release(AGC_WEIGHTED_WORKSPACE *workspace);
/* Weighted Mapping builds native RAW14 output LUTs; no Gray8 quantization.
 * Redistribution fills only originally populated bins, up to the Clip cap;
 * excess without capacity is discarded. Bright percentile is recalculated
 * from that processed histogram before applying Bright Weight: first integer DN
 * with CDF > P (P100 uses the last retained DN), without percentile interpolation.
 * Bright intervals use k >= threshold. The Classic LUT path has no allocation
 * or automatic ACI/THL/THH.
 * Workspace must start zeroed. LUT smoothing blends LUTs before Gain Limit.
 * Gain Limit compensates histogram-weighted mean brightness, not a discrete
 * median DN. Compensation uses the same temporal weight in Q8 precision.
 * Optional simple_mode runs AFTER Gain Limit: compose a 5..211-Gray brightness
 * curve into the LUT; Mild/Strong add one bounded guided residual from input RAW.
 * The image stays native RAW14 throughout; Gray values only specify controls.
 * Local radius=24 px; S=max(P99-P1,32 DN); epsilon=max(2,0.12*S) DN.
 * Mild gain=min(0.8,510/S), limit +/-32 Gray; Strong=min(1.2,765/S), +/-44.
 * Spatial modes clip final output to 3..246 Gray; Gain Limit does not cap this
 * added correction. Off and Brightness need no spatial scratch allocation.
 * Mild/Strong lazily allocate 4 float planes (4.69 MiB at 640x480), reused until
 * workspace_release. No extra pixel history, coring, RMS gate or tanh is used.
 * Fast Local uses Mild settings with a 4x4 pooled guide and interpolated
 * coefficients; the output stays full resolution.
 * Local Mean uses one low-resolution box filter and fixed-point detail gain.
 * Gamma stays in its separate ISP path. Only the existing LUT/gain tracks time.
 * Calibration/manual-open events reset the existing LUT/gain history.
 * Input is preserved unless input == output. */
int agc_weighted_apply_u14(const uint16_t *input, size_t input_stride,
    uint16_t *output, size_t output_stride, uint32_t width, uint32_t height,
    AGC_WEIGHTED_WORKSPACE *workspace, const AGC_WEIGHTED_PARAMS *params);
#endif
