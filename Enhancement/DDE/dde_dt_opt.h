/* =====================================================================
 * dde_dt_opt.h  ─  Domain Transform Filter 
 * ===================================================================== */

#ifndef DDE_DT_OPT_H
#define DDE_DT_OPT_H

#include <stdint.h>


uint32_t dde_dt_get_workbuf_size(uint32_t width, uint32_t height);


/* ---------------------------------------------------------------------
 * DDE 主函式 (in-place：輸出直接覆寫 p_img)
 *
 * @param p_img       有效區域起始指標, 呼叫端須自行加上 ROI 偏移，
 *                    傳入 Y plane 的 CPU VA，非 physical address。
 * @param width       有效區域寬度
 * @param height      有效區域高度
 * @param line_stride 每列的 byte 數（含 padding）
 * @param sigma_s     空間標準差
 * @param sigma_r     強度標準差
 * @param gain        細節層增益
 * @param num_iters   迭代次數
 * @param out_max     輸出上限 (Y8=255)
 * -------------------------------------------------------------------*/
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
    void     *pWorkBuf);

#endif /* DDE_DT_OPT_H */
