#include "Enhancement/DDE/dde_frame.h"
#include "Enhancement/DDE/dde.h"
#include "Enhancement/DDE/dde_dt_opt.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t image[8192];
static unsigned maps, unmaps, flushes;
static int fail_map, fail_flush, fail_unmap;
static const UINTPTR physical_base = 0x1000;
UINTPTR hd_common_mem_blk2pa(UINTPTR block) { (void)block; return physical_base; }
void *hd_common_mem_mmap(int type, UINTPTR base, UINT32 bytes)
{
    (void)type;
    assert(base == physical_base && bytes <= sizeof(image));
    maps++;
    return fail_map ? (void *)(uintptr_t)-1 : image;
}
int hd_common_mem_flush_cache(void *va, UINT32 bytes)
{
    assert(va == image && bytes <= sizeof(image));
    flushes++;
    return fail_flush ? -1 : HD_OK;
}
int hd_common_mem_munmap(void *va, UINT32 bytes)
{
    assert(va == image && bytes <= sizeof(image));
    unmaps++;
    return fail_unmap ? -1 : HD_OK;
}

int main(void)
{
    DDE_PARAMS p = {1, 21, 51, 200, 2, 1};
    HD_VIDEO_FRAME f = {HD_VIDEO_PXLFMT_YUV420, {0x1010}, {24}, {19, 17}, 1};
    uint8_t before[sizeof(image)], expected[sizeof(image)];
    assert(dde_set_params(&p) == 0);
    for (int scale = 1; scale <= 2; scale++) {
      p.base_scale=scale; assert(dde_set_params(&p)==0);
      for (unsigned pass = 0; pass < 3; pass++) {
        if (pass == 0) { f.dim.w = 19; f.dim.h = 17; f.loff[0] = 24; }
        if (pass == 1) { f.dim.w = 33; f.dim.h = 35; f.loff[0] = 40; }
        if (pass == 2) { f.dim.w = 1; f.dim.h = 1; f.loff[0] = 8; }
        memset(image, 173, sizeof(image));
        for (unsigned y = 0; y < f.dim.h; y++)
            for (unsigned x = 0; x < f.dim.w; x++)
                image[16 + y * f.loff[0] + x] = (uint8_t)(x * 13 + y * 7);
        memcpy(expected, image, sizeof(image));
        void *work = malloc(dde_dt_scaled_workbuf_size(f.dim.w, f.dim.h, p.base_scale));
        assert(work);
        assert(DDE_DT_Optimized_Scaled(expected + 16, f.dim.w, f.dim.h, f.loff[0],
                               21, 51, 2, 2, 255, 0, p.base_scale, work) == 0);
        free(work);
        assert(dde_process_frame(&f) == 0);
        /* Entire allocation comparison also covers UV, prefix and row padding. */
        assert(memcmp(image, expected, sizeof(image)) == 0);
    }
    }
    assert(maps == 6 && unmaps == 6 && flushes == 12);
    memcpy(before, image, sizeof(image));
    p.enable = 0;
    assert(dde_set_params(&p) == 0 && dde_process_frame(NULL) == 0);
    assert(maps == 6 && memcmp(image, before, sizeof(image)) == 0);
    p.enable = 1;
    assert(dde_set_params(&p) == 0);
    f.loff[0] = 0;
    assert(dde_process_frame(&f) < 0 && maps == 6);
    f.loff[0] = 8;
    fail_map = 1;
    assert(dde_process_frame(&f) < 0 && unmaps == 6);
    fail_map = 0;
    fail_flush = 1;
    assert(dde_process_frame(&f) < 0 && unmaps == 7);
    assert(memcmp(image, before, sizeof(image)) == 0);
    fail_flush = 0;
    fail_unmap = 1;
    assert(dde_process_frame(&f) < 0 && unmaps == 8);
    dde_frame_release();
    dde_frame_release();
    puts("PASS: real frame wrapper; Y8 only; UV/padding preserved; resize/release; disabled/error paths.");
    return 0;
}
