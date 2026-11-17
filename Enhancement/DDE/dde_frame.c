#include "dde_frame.h"
#include "dde.h"
#include <stdint.h>
#include <stdlib.h>
#include "dde_dt_opt.h"
static void *workspace;
static uint32_t capacity;
/* Called only by display worker; release only after that worker is joined. */
void dde_frame_release(void) { free(workspace); workspace = NULL; capacity = 0; }
int dde_process_frame(const HD_VIDEO_FRAME *f)
{
    DDE_PARAMS p;
    UINTPTR base, va;
    uint64_t offset, bytes;
    int result;
    dde_get_params(&p);
    if (!p.enable) return 0;
    if (!f || f->pxlfmt != HD_VIDEO_PXLFMT_YUV420 || !f->phy_addr[0] ||
        !f->dim.w || !f->dim.h || f->loff[0] < f->dim.w) return -1;
    {
        uint32_t required = dde_dt_scaled_workbuf_size(f->dim.w, f->dim.h, p.base_scale);
        if (!required) return -1;
        if (capacity < required) {
            void *next = realloc(workspace, required);
            if (!next) return -1;
            workspace = next;
            capacity = required;
        }
    }
    base = (f->blk && f->blk != HD_COMMON_MEM_VB_INVALID_BLK) ?
        hd_common_mem_blk2pa(f->blk) : 0;
    if (!base || base > f->phy_addr[0]) base = f->phy_addr[0];
    offset = f->phy_addr[0] - base;
    if (offset > UINT32_MAX) return -1;
    bytes = offset + (uint64_t)f->loff[0] * f->dim.h;
    if (bytes > UINT32_MAX) return -1;
    va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, base, (UINT32)bytes);
    if (!va || va == (UINTPTR)-1) return -1;
    result = hd_common_mem_flush_cache((void *)va, (UINT32)bytes);
    if (result == HD_OK) {
        result = DDE_DT_Optimized_Scaled((uint8_t *)va + (size_t)offset,
            f->dim.w, f->dim.h, f->loff[0],
            (float)p.sigma_s, (float)p.sigma_r, p.gain_percent / 100.0f,
            p.num_iters, 255, 0, p.base_scale, workspace);
        if (hd_common_mem_flush_cache((void *)va, (UINT32)bytes) != HD_OK) result = -1;
    }
    if (hd_common_mem_munmap((void *)va, (UINT32)bytes) != HD_OK) result = -1;
    return result;
}
