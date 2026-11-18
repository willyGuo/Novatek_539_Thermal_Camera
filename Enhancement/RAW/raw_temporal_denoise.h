#ifndef RAW_TEMPORAL_DENOISE_H
#define RAW_TEMPORAL_DENOISE_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t enable;
    uint32_t threshold_dn;  /* Native RAW DN versus filtered Q8 history; no scaling. */
    uint32_t history_shift; /* 1: previous 50%, 2: 75%, 3: 87.5%. */
    uint32_t spatial_enable;
    uint32_t spatial_threshold_dn;
    uint32_t dual_speed_enable;
    uint32_t motion_threshold_dn;
    uint32_t motion_strong_threshold_dn;
    uint32_t motion_hysteresis_dn;
    uint32_t motion_hold_frames;
    uint32_t spatial_mode; /* 0: strict all-neighbor gate; 1: selective neighbors. */
} RAW_TEMPORAL_DENOISE_PARAMS;
#define RAW_TEMPORAL_DENOISE_DEFAULTS {0U, 12U, 2U, 1U, 8U, 1U, 3U, 8U, 1U, 3U, 1U}
enum {
    RAW_TEMPORAL_ERROR_PARAMS = -1,
    RAW_TEMPORAL_ERROR_LAYOUT = -2,
    RAW_TEMPORAL_ERROR_MEMORY = -3
};

typedef struct {
    uint32_t *history_q8;
    size_t capacity;
    uint32_t width, height;
    uint64_t timestamp_us;
    int valid;
    RAW_TEMPORAL_DENOISE_PARAMS params;
    uint32_t *slow_history_q8;
    uint16_t *motion; /* High byte: 0 slow, 1 fast, 2 raw; low byte: hold. */
    size_t dual_capacity;
    uint16_t *spatial_row;
    size_t row_capacity;
} RAW_TEMPORAL_DENOISE_STATE;

void raw_temporal_denoise_get_params(RAW_TEMPORAL_DENOISE_PARAMS *p);
int raw_temporal_denoise_validate(const RAW_TEMPORAL_DENOISE_PARAMS *p);
int raw_temporal_denoise_set_params(const RAW_TEMPORAL_DENOISE_PARAMS *p);
void raw_temporal_denoise_reset(RAW_TEMPORAL_DENOISE_STATE *s);
void raw_temporal_denoise_destroy(RAW_TEMPORAL_DENOISE_STATE *s);
/* CPU in-place processing of a dedicated RAW16-LSB buffer. Sensor DN is
 * nominally RAW14; upstream corrections may extend it to 0..65535. Preserve
 * those DN without clipping, masking or shifting. UINT32 Q8 history and the
 * largest weighted sum (k=4) safely cover the complete UINT16 domain.
 * State must start zeroed; worker alone owns it. Lazy history/row allocation.
 * enable is the master switch. New flags off preserve the legacy IIR.
 * Spatial uses original N/S/E/W/C, bypasses borders, retains Q8 fractions.
 * Strict mode bypasses the center when any neighbor exceeds the threshold.
 * Selective mode replaces each rejected neighbor with C in (4C+N+S+E+W)/8;
 * it does not mix across rejected edges or allocate another image buffer.
 * Mode/reset dispatch and Q8 constants are prepared once per frame. Spatial
 * interior spans avoid per-pixel boundary checks; borders remain copy-through
 * spatially and still receive the selected temporal processing.
 * Dual speed uses fixed previous weights 50% / 93.75%; legacy threshold/shift
 * apply only when dual_speed_enable is off. Strong motion outputs original RAW.
 * Returns RAW_TEMPORAL_ERROR_* on error, 0 disabled, 1 filtered,
 * 2 initialized/reset (spatial-only if enabled, otherwise copy-through).
 * No DN rescaling. Gaps >250ms, backwards/repeated timestamps,
 * size/parameter changes reset history. Invalid input leaves pixels/state intact. */
int raw_temporal_denoise_process(RAW_TEMPORAL_DENOISE_STATE *s,
    uint16_t *pixels, size_t stride, uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p);
/* Same filtering/state semantics, reading source directly into a dedicated
 * output (no staging memcpy). Input/output may be identical with equal stride
 * or must have disjoint ranges; partial overlaps are rejected before writes.
 * Separate input and row padding stay untouched. Disabled: reset state and
 * return 0 without writing output. Invalid layout/allocation failure writes
 * neither pixels nor filter history. Caller owns cache sync and frame lease. */
int raw_temporal_denoise_process_to(RAW_TEMPORAL_DENOISE_STATE *s,
    const uint16_t *input, size_t input_stride, uint16_t *output, size_t output_stride,
    uint32_t width, uint32_t height, uint64_t timestamp_us,
    const RAW_TEMPORAL_DENOISE_PARAMS *p);
#endif
