/* =====================================================================
 * dde_dt_opt.c  ─  Domain Transform Filter 為基礎的 DDE 細節增強
 *
 * 有效區域 Y8 plane，stride 單位為 bytes
 * ===================================================================== */

#include "dde_dt_opt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PERCENTILE_BINS 4096
#define DIFF_LUT_SIZE 256


#ifndef DDE_VTILE
#define DDE_VTILE 16
#endif


#ifndef DDE_USING_MAPPING
#define DDE_USING_MAPPING 0
#endif


#ifndef DDE_MAX_STRETCH
#define DDE_MAX_STRETCH 2.5f
#endif

#if DDE_USING_MAPPING
/* =====================================================================
 * 內部輔助：直方圖百分位數
 *
 * 使用外部傳入的 hist_buf 而非區域陣列，避免在 RTOS 的小 stack 上
 * ===================================================================== */
static void compute_percentiles(const float *data, int length,
                                float min_val, float max_val,
                                float p_low, float p_high,
                                float *v_min, float *v_max,
                                int *hist_buf)
{
    // 1. 找出極值 
    float range = max_val - min_val;
    if (range <= 0.0001f) {
        *v_min = min_val;
        *v_max = max_val;
        return;
    }

    memset(hist_buf, 0, PERCENTILE_BINS * sizeof(int));

    /* 2. 建立直方圖 */
    for (int i = 0; i < length; i++) {
        int bin = (int)(((data[i] - min_val) / range) * (PERCENTILE_BINS - 1));
        if (bin < 0) bin = 0;
        if (bin >= PERCENTILE_BINS) bin = PERCENTILE_BINS - 1;
        hist_buf[bin]++;
    }

    int target_low  = (int)(((float)length * p_low)  / 100.0f);
    int target_high = (int)(((float)length * p_high) / 100.0f);

    /* 3. 低百分位 */
    int accum = 0;
    *v_min = min_val;
    for (int i = 0; i < PERCENTILE_BINS; i++) {
        accum += hist_buf[i];
        if (accum >= target_low) {
            *v_min = min_val + ((float)i / (PERCENTILE_BINS - 1)) * range;
            break;
        }
    }

    /* 4. 高百分位 */
    accum = 0;
    *v_max = max_val;
    for (int i = 0; i < PERCENTILE_BINS; i++) {
        accum += hist_buf[i];
        if (accum >= target_high) {
            *v_max = min_val + ((float)i / (PERCENTILE_BINS - 1)) * range;
            break;
        }
    }
}
#endif /* DDE_USING_MAPPING */

/* =====================================================================
 * 取得工作緩衝區大小
 * ===================================================================== */
uint32_t dde_dt_get_workbuf_size(uint32_t width, uint32_t height)
{
    if (!width || !height || width > 4096 || height > 4096) return 0;
    uint32_t num_pixels = width * height;
    uint32_t max_dim    = (width > height) ? width : height;

    uint32_t floats_needed = (num_pixels * 2)									  
                           + DIFF_LUT_SIZE
                           + (max_dim * DDE_VTILE * 3)
                           + DDE_VTILE;

    /* Optional mapping reuses out_h for a 4096-bin histogram. */
    if (num_pixels < PERCENTILE_BINS) floats_needed += PERCENTILE_BINS - num_pixels;
    return floats_needed * (uint32_t)sizeof(float);
}


/* =====================================================================
 * DDE 主函式
 * ===================================================================== */
int DDE_DT_Optimized(
    uint8_t *p_img,
    uint32_t  width,
    uint32_t  height,
    uint32_t  line_stride,
    float     sigma_s,
    float     sigma_r,
    float     gain,
    int       num_iters,
    uint8_t  out_max,
    uint8_t  out_black,
    void     *pWorkBuf)
{
    if (!p_img || !pWorkBuf || width == 0 || height == 0)  return -1;
    if (!dde_dt_get_workbuf_size(width, height) || line_stride < width ||
        (uint64_t)line_stride * height > UINT32_MAX) return -1;
    if (!isfinite(sigma_s) || !isfinite(sigma_r) || !isfinite(gain) ||
        sigma_s < 0.01f || sigma_s > 4096.0f || sigma_r < 0.01f ||
        sigma_r > 255.0f || gain > 10.0f) return -3;
    if (out_max <= out_black)                              return -2;
    if (!(sigma_s > 0.0f) || !(sigma_r > 0.0f))            return -3;
    if (num_iters < 1 || num_iters > 8)                    return -4;
    if (!(gain >= 0.0f))                                   return -5;

    const int num_pixels = (int)(width * height);
    const int max_dim    = (int)((width > height) ? width : height);

    /* ── Step 0: 記憶體池劃分 ─────────────────────────────────────
     * 單一連續配置，無動態記憶體、無大型 static 陣列。
     * ------------------------------------------------------------- */
    float *base       = (float *)pWorkBuf;                      /* W * H            */
    float *out_h      = base     + num_pixels;                  /* W * H            */
    float *diff_lut   = out_h    + (num_pixels < PERCENTILE_BINS ? PERCENTILE_BINS : num_pixels);                  /* DIFF_LUT_SIZE    */
																					  																					  																																							  

    /* 分塊暫存區，索引方式為 tile-major: [ i * DDE_VTILE + t ] */
    float *gtile      = diff_lut + DIFF_LUT_SIZE;               /* max_dim * VTILE  */
    float *a_t        = gtile + (size_t)max_dim * DDE_VTILE;    /* max_dim * VTILE  */
    float *src_t      = a_t   + (size_t)max_dim * DDE_VTILE;    /* max_dim * VTILE  */
    float *ybrow      = src_t + (size_t)max_dim * DDE_VTILE;    /* DDE_VTILE        */
    float *dst_t      = gtile;

    /* ── Step 1: 初始化 Base 層 ───────────────────────────────────
     * 轉為連續 float 平面。
     * ------------------------------------------------------------- */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            base[y * width + x] = (float)p_img[y * line_stride + x];
        }
    }

    /* ── Step 2: Domain Transform 迭代濾波 ───────────────────────── */
    const float denom = sqrtf(powf(4.0f, (float)num_iters) - 1.0f);

    for (int iter = 0; iter < num_iters; iter++) {

        /* 讓 N 次遞迴濾波的變異數疊加後逼近單一高斯 */
        float sigma_s_iter = sigma_s * sqrtf(3.0f)
                           * powf(2.0f, (float)(num_iters - iter - 1)) / denom;

        /* ── 建立本次迭代的係數 LUT ──
         * a(d) = exp( -sqrt(2)/sigma_s * sqrt( 1 + (sigma_s/sigma_r)^2 * d^2 ) )
        */
        float factor        = (sigma_s_iter / sigma_r) * (sigma_s_iter / sigma_r);
        float sqrt2_over_ss = (float)(sqrt(2.0) / sigma_s_iter);

        for (int d = 0; d < DIFF_LUT_SIZE; d++) {
            float val = 1.0f + factor * (float)d * (float)d;
            diff_lut[d] = expf(-sqrt2_over_ss * sqrtf(val));
        }

        /* ── A. 水平濾波  ──
         * ----------------------------------------------------------- */
        for (uint32_t r0 = 0; r0 < height; r0 += DDE_VTILE) {
            uint32_t th = height - r0;
            if (th > DDE_VTILE) th = DDE_VTILE;

            /* A-1. 各列的起始指標*/
            const uint8_t *prow[DDE_VTILE];
            const float    *srow[DDE_VTILE];
            float          *drow[DDE_VTILE];
            for (uint32_t t = 0; t < th; t++) {
                prow[t] = p_img + (size_t)(r0 + t) * line_stride;
                srow[t] = base  + (size_t)(r0 + t) * width;
                drow[t] = out_h + (size_t)(r0 + t) * width;
            }

            /* A-2. 遞迴係數 a_t[i][t], 取自原圖 p_img */
            for (uint32_t i = 0; i + 1 < width; i++) {
                float *at = a_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < th; t++) {
                    int d = (int)fabsf((float)prow[t][i + 1] - (float)prow[t][i]);
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    at[t] = diff_lut[d];
                }
            }

            /* A-3. 正向遞迴，直接寫入 out_h */
            for (uint32_t t = 0; t < th; t++) drow[t][0] = srow[t][0];
            for (uint32_t i = 1; i < width; i++) {
                const float *at = a_t + (size_t)(i - 1) * DDE_VTILE;
                for (uint32_t t = 0; t < th; t++) {
                    drow[t][i] = (1.0f - at[t]) * srow[t][i] + at[t] * drow[t][i - 1];
                }
            }

            /* A-4. 反向遞迴 + 雙向平均 */
            for (uint32_t t = 0; t < th; t++) {
                ybrow[t] = srow[t][width - 1];
                drow[t][width - 1] = (drow[t][width - 1] + ybrow[t]) * 0.5f;
            }
            for (int i = (int)width - 2; i >= 0; i--) {
                const float *at = a_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < th; t++) {
                    ybrow[t] = (1.0f - at[t]) * srow[t][i] + at[t] * ybrow[t];
                    drow[t][i] = (drow[t][i] + ybrow[t]) * 0.5f;
                }
            }
        }

        /* ── B. 垂直濾波  ──
         * ----------------------------------------------------------- */
        for (uint32_t c0 = 0; c0 < width; c0 += DDE_VTILE) {

            uint32_t tw = width - c0;
            if (tw > DDE_VTILE) tw = DDE_VTILE;

            for (uint32_t i = 0; i < height; i++) {
                const uint8_t *pr = p_img + (size_t)i * line_stride + c0;
                const float    *sr = out_h + (size_t)i * width + c0;
                float *gt = gtile + (size_t)i * DDE_VTILE;
                float *st = src_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    gt[t] = (float)pr[t];
                    st[t] = sr[t];
                }
            }

            /* B-2. 遞迴係數 a_t[i][t]*/
            for (uint32_t i = 0; i + 1 < height; i++) {
                const float *g0 = gtile + (size_t)i * DDE_VTILE;
                const float *g1 = gtile + (size_t)(i + 1) * DDE_VTILE;
                float *at = a_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    int d = (int)fabsf(g1[t] - g0[t]);
                    if (d > (DIFF_LUT_SIZE - 1)) d = DIFF_LUT_SIZE - 1;
                    at[t] = diff_lut[d];
                }
            }

            /* B-3. 正向遞迴 */
            {
                const float *s0 = src_t;
                float *d0 = dst_t;
                for (uint32_t t = 0; t < tw; t++) d0[t] = s0[t];
            }
            for (uint32_t i = 1; i < height; i++) {
                const float *at = a_t   + (size_t)(i - 1) * DDE_VTILE;
                const float *st = src_t + (size_t)i * DDE_VTILE;
                const float *dp = dst_t + (size_t)(i - 1) * DDE_VTILE;
                float       *dc = dst_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    dc[t] = (1.0f - at[t]) * st[t] + at[t] * dp[t];
                }
            }

            /* B-4. 反向遞迴 + 雙向平均。 */
            {
                size_t off = (size_t)(height - 1) * DDE_VTILE;
                const float *st = src_t + off;
                float       *dc = dst_t + off;
                for (uint32_t t = 0; t < tw; t++) {
                    ybrow[t] = st[t];
                    dc[t] = (dc[t] + ybrow[t]) * 0.5f;
                }
            }
            for (int i = (int)height - 2; i >= 0; i--) {
                const float *at = a_t   + (size_t)i * DDE_VTILE;
                const float *st = src_t + (size_t)i * DDE_VTILE;
                float       *dc = dst_t + (size_t)i * DDE_VTILE;
                for (uint32_t t = 0; t < tw; t++) {
                    ybrow[t] = (1.0f - at[t]) * st[t] + at[t] * ybrow[t];
                    dc[t] = (dc[t] + ybrow[t]) * 0.5f;
                }
            }

            /* B-5. 寫回 Base 層 */
            for (uint32_t i = 0; i < height; i++) {
                const float *dc = dst_t + (size_t)i * DDE_VTILE;
                float *br = base + (size_t)i * width + c0;
                for (uint32_t t = 0; t < tw; t++) br[t] = dc[t];
            }
        }
    }

    /* ── Step 3: 細節萃取與增益重組 ───────────────────────────────
     *   detail = I - B ,  I_comb = B + gain * detail
     * ------------------------------------------------------------- */
#if DDE_USING_MAPPING
        float scan_min =  INFINITY;
        float scan_max = -INFINITY;
#endif

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            size_t idx = (size_t)y * width + x;
            float original = (float)p_img[(size_t)y * line_stride + x];
            float detail   = original - base[idx];
            float v        = base[idx] + detail * gain;
            base[idx] = v;

#if DDE_USING_MAPPING
            if (v < scan_min) scan_min = v;
            if (v > scan_max) scan_max = v;
#endif
        }
    }
    

    /* ── Step 4: 百分位數與線性映射參數 ──────────────────────────
     * ------------------------------------------------------------- */

#if DDE_USING_MAPPING
        float v_min, v_max;
        int *hist_buf = (int *)out_h;

        compute_percentiles(base, num_pixels, scan_min, scan_max, 1.0f, 99.0f, &v_min, &v_max, hist_buf);

        const float f_out_max   = (float)out_max;
        const float f_out_black = (float)out_black;

        float win = f_out_max - f_out_black;
        if (win < 1.0f) win = 1.0f;             /* 防呆 */

        float span  = v_max - v_min;
        float scale = DDE_MAX_STRETCH;
        if (span > 1e-3f) {
            scale = win / span;
            if (scale > DDE_MAX_STRETCH) {      /* 僅設上限，不禁止擴張 */
                scale = DDE_MAX_STRETCH;
            }
        }

        float pad    = (win - span * scale) * 0.5f;
        if (pad < 0.0f) pad = 0.0f;             
        float offset = f_out_black + pad - v_min * scale;

#else
        // ── 除錯路徑：不做百分位、不做映射 ── 
        const float f_out_max   = (float)out_max;
        const float f_out_black = out_black;          
        const float scale  = 1.0f;
        const float offset = 0.0f;

#endif /* DDE_USING_MAPPING */


    /* ── Step 5: 寫回原影像 ─────────────────────────────────────── */
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            float val = base[(size_t)y * width + x] * scale + offset;

            if (!(val > f_out_black)) val = f_out_black;
            if (val > f_out_max)      val = f_out_max;

            /* 四捨五入取代直接截斷 */
            p_img[(size_t)y * line_stride + x] = (uint8_t)(val + 0.5f);
        }
    }

    return 0;
}
