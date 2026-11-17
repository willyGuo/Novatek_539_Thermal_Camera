#ifndef DDE_DT_OPT_H
#define DDE_DT_OPT_H

#include <stdint.h>

/* Bytes required for malloc-aligned workspace; 0 for invalid dimensions.
 * Supported dimensions: 1..4096 in each axis. Workspace is caller-owned.
 */
uint32_t dde_dt_get_workbuf_size(uint32_t width, uint32_t height);
uint32_t dde_dt_scaled_workbuf_size(uint32_t width, uint32_t height, uint32_t base_scale);
/* Build-selected backend, for checking that the target enables SIMD. */
const char *dde_dt_backend_name(void);

/* DDE_1106 domain-transform detail enhancement, native Y8 in-place.
 * line_stride is in bytes and must be >= width. Row padding is untouched.
 * sigma_s: spatial pixels; sigma_r: Y8 DN; gain: detail multiplier (0..10).
 * num_iters: 1..8 directional passes with paired sigma by default.
 * out_black/out_max: fixed Y8 clipping bounds (normally 0/255).
 * No automatic percentile mapping in the production/default build.
 * Returns 0 on success; invalid arguments return a negative code before writes.
 */
int DDE_DT_Optimized(
    uint8_t *p_img, uint32_t width, uint32_t height, uint32_t line_stride,
    float sigma_s, float sigma_r, float gain, int num_iters,
    uint8_t out_max, uint8_t out_black, void *pWorkBuf);

/* base_scale=1: original full-resolution algorithm.
 * base_scale=2: rounded 2x2 Y8 guide, float base at half width/height,
 * sigma_s/2, bilinear upsample and full-resolution detail recombination.
 * No intermediate base quantization. Requires the fused/no-mapping build.
 * -6 rejects unsupported scale/build before writing the image.
 */
int DDE_DT_Optimized_Scaled(
    uint8_t *p_img, uint32_t width, uint32_t height, uint32_t line_stride,
    float sigma_s, float sigma_r, float gain, int num_iters,
    uint8_t out_max, uint8_t out_black, uint32_t base_scale, void *pWorkBuf);

#endif
