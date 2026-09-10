#include "clahe_frame.h"
#include "clahe.h"
#include <stdint.h>
static CLAHE_WORKSPACE workspace;
/* Called only by display worker; release only after that worker is joined. */
void clahe_frame_release(void) { clahe_workspace_release(&workspace); }
int clahe_process_frame(const HD_VIDEO_FRAME *f)
{
    CLAHE_PARAMS p;
    UINTPTR base, va;
    uint64_t offset, bytes;
    int result;
    clahe_get_params(&p);
    if (!p.enable) return 0;
    if (!f || f->pxlfmt != HD_VIDEO_PXLFMT_YUV420 || !f->phy_addr[0] ||
        !f->dim.w || !f->dim.h || f->loff[0] < f->dim.w) return -1;
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
        result = clahe_apply_y8((uint8_t *)(va + offset), f->loff[0],
            f->dim.w, f->dim.h, &p, &workspace);
        if (hd_common_mem_flush_cache((void *)va, (UINT32)bytes) != HD_OK) result = -1;
    }
    if (hd_common_mem_munmap((void *)va, (UINT32)bytes) != HD_OK) result = -1;
    return result;
}
