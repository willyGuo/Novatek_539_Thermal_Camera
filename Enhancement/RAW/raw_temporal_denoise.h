#ifndef RAW_TEMPORAL_DENOISE_H
#define RAW_TEMPORAL_DENOISE_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t enable;
    uint32_t threshold_dn;  /* Native RAW DN versus filtered Q8 history; no scaling. */
    uint32_t history_shift; /* 1: previous 50%, 2: 75%, 3: 87.5%. */
} RAW_TEMPORAL_DENOISE_PARAMS;
#define RAW_TEMPORAL_DENOISE_DEFAULTS {0U, 12U, 2U}
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
} RAW_TEMPORAL_DENOISE_STATE;

void raw_temporal_denoise_get_params(RAW_TEMPORAL_DENOISE_PARAMS *p);
int raw_temporal_denoise_validate(const RAW_TEMPORAL_DENOISE_PARAMS *p);
int raw_temporal_denoise_set_params(const RAW_TEMPORAL_DENOISE_PARAMS *p);
void raw_temporal_denoise_reset(RAW_TEMPORAL_DENOISE_STATE *s);
void raw_temporal_denoise_destroy(RAW_TEMPORAL_DENOISE_STATE *s);
/* CPU in-place processing of a dedicated RAW16-LSB buffer. Sensor DN is
 * nominally RAW14; upstream corrections may extend it to 0..65535. Preserve
 * those DN without clipping, masking or shifting. UINT32 Q8 history and the
 * largest weighted sum (k=3) safely cover the complete UINT16 domain.
 * State must start zeroed; worker alone owns it. Lazy history allocation.
 * Returns RAW_TEMPORAL_ERROR_* on error, 0 disabled, 1 filtered,
 * 2 initialized/reset (copy-through).
 * No spatial filter or bit shift. Gaps >250ms, backwards/repeated timestamps,
 * size/parameter changes reset history. Invalid input leaves pixels/state intact. */
int raw_temporal_denoise_process(RAW_TEMPORAL_DENOISE_STATE *s,
    uint16_t *pixels, size_t stride, uint32_t width, uint32_t height,
    uint64_t timestamp_us, const RAW_TEMPORAL_DENOISE_PARAMS *p);
#endif
