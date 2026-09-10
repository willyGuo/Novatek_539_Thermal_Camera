#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "libive/libive.h"

#include "pseudo.h"

#define PSEUDO_POINT_NUM 32

static UINT8 pseudo_u[256];
static UINT8 pseudo_v[256];

static PSEUDO_TYPE pseudo_type = PSEUDO_TYPE_NORMAL;
// static const UINT8 pseudo_palettes[3][7][3] = {
//     { /* Reverse: white to black. */
//         {255, 255, 255}, {213, 213, 213}, {170, 170, 170},
//         {128, 128, 128}, {85, 85, 85}, {43, 43, 43}, {0, 0, 0}
//     },
//     { /* Rainbow: violet to red. */
//         {238, 130, 238}, {75, 0, 130}, {0, 0, 255},
//         {0, 255, 0}, {255, 255, 0}, {255, 165, 0}, {255, 0, 0}
//     },
//     { /* Hot Iron: black to white. */
//         {0, 0, 0}, {64, 0, 64}, {128, 0, 128},
//         {255, 0, 0}, {255, 165, 0}, {255, 255, 0}, {255, 255, 255}
//     }
// };

static const UINT8 pseudo_palettes[3][PSEUDO_POINT_NUM][3] = {

    /* =========================================================
     * 0. Reverse : White -> Black
     * ========================================================= */
    {
        {255,255,255}, {247,247,247}, {239,239,239}, {230,230,230},
        {222,222,222}, {214,214,214}, {206,206,206}, {197,197,197},
        {189,189,189}, {181,181,181}, {173,173,173}, {165,165,165},
        {156,156,156}, {148,148,148}, {140,140,140}, {132,132,132},
        {123,123,123}, {115,115,115}, {107,107,107}, { 99, 99, 99},
        { 90, 90, 90}, { 82, 82, 82}, { 74, 74, 74}, { 66, 66, 66},
        { 58, 58, 58}, { 49, 49, 49}, { 41, 41, 41}, { 33, 33, 33},
        { 25, 25, 25}, { 16, 16, 16}, {  8,  8,  8}, {  0,  0,  0}
    },

    /* =========================================================
     * 1. Jet
     * Dark Blue -> Blue -> Cyan -> Green -> Yellow -> Red
     * ========================================================= */
    {
        {  0,   0, 128},{  0,   0, 160},{  0,   0, 192},{  0,   0, 224},{  0,   0, 255},

        {  0,  32, 255},{  0,  64, 255},{  0,  96, 255},{  0, 128, 255},{  0, 160, 255},{  0, 192, 255},{  0, 224, 255},

        {  0, 255, 255},{ 32, 255, 224},{ 64, 255, 192},{ 96, 255, 160},

        {128, 255, 128},{160, 255,  96},{192, 255,  64},{224, 255,  32},

        {255, 255,   0},{255, 224,   0},{255, 192,   0},{255, 160,   0},{255, 128,   0},{255,  96,   0},{255,  64,   0},{255,  32,   0},

        {255,   0,   0},{224,   0,   0},{192,   0,   0},{128,   0,   0}
    },

    /* =========================================================
     * 2. Hot Iron
     * Black -> Purple -> Dark Red -> Red -> Orange
     * -> Yellow -> White
     * ========================================================= */
    {
        {  0,   0,   0},{ 12,   0,  16},{ 24,   0,  32},{ 36,   0,  48},{ 48,   0,  64},{ 64,   0,  80},{ 80,   0,  96},{ 96,   0, 112},

        {112,   0, 128},{128,   0, 128},{144,   0, 112},{160,   0,  96},{176,   0,  80},{192,   0,  64},{208,   0,  48},{224,   0,  32},

        {240,   0,  16},{255,   0,   0},{255,  32,   0},{255,  64,   0},{255,  96,   0},{255, 128,   0},{255, 160,   0},{255, 192,   0},

        {255, 208,   0},{255, 224,   0},{255, 240,   0},{255, 255,   0},{255, 255,  64},{255, 255, 128},{255, 255, 192},{255, 255, 255}
    }
};


typedef struct {
	HD_COMMON_MEM_VB_BLK Y_plane_blk;
	UINTPTR Y_plane_pa;
	UINTPTR Y_plane_va;

	HD_COMMON_MEM_VB_BLK Y_out_blk;
	UINTPTR Y_out_pa;
	UINTPTR Y_out_va;

	int Y_out_size;

} Pseudo_MEM;


static Pseudo_MEM pseudo_buffer[1];
static int pseudo_ive_inited;
static pthread_mutex_t pseudo_lock = PTHREAD_MUTEX_INITIALIZER;
static int pseudo_requested_mode = PSEUDO_TYPE_NORMAL;
static int pseudo_frame_size;
static HD_RESULT pseudo_uninit_locked(void);

const char *pseudo_mode_name(int mode)
{
    static const char * const names[] = {"Black/White", "White/Black", "Color (Jet)", "Iron"};
    return mode >= 0 && mode <= 3 ? names[mode] : "Invalid";
}

int pseudo_get_mode(void)
{
    int mode;
    pthread_mutex_lock(&pseudo_lock);
    mode = (int)pseudo_type;
    pthread_mutex_unlock(&pseudo_lock);
    return mode;
}

int pseudo_request_mode(int mode)
{
    if (mode < 0 || mode > 3) return -1;
    pthread_mutex_lock(&pseudo_lock);
    pseudo_requested_mode = mode;
    pthread_mutex_unlock(&pseudo_lock);
    return 0;
}

int pseudo_set_mode(int mode)
{
    int ret = -1;
    pthread_mutex_lock(&pseudo_lock);
    if (mode >= 0 && mode <= 3 && !pseudo_ive_inited) {
        pseudo_type = (PSEUDO_TYPE)mode;
        pseudo_requested_mode = mode;
        ret = 0;
    }
    pthread_mutex_unlock(&pseudo_lock);
    return ret;
}

/* Release only resources that were successfully acquired. */
static HD_RESULT pseudo_release_buffer(UINTPTR *va, UINTPTR *pa,
    HD_COMMON_MEM_VB_BLK *blk, UINT32 size, const char *name)
{
    HD_RESULT ret;
    if (*va != 0) {
        ret = hd_common_mem_munmap((void *)*va, size);
        if (ret != HD_OK) {
            printf("Pseudo %s munmap failed=%d\n", name, ret);
            return ret;
        }
        *va = 0;
    }
    if (*blk != 0 && *blk != HD_COMMON_MEM_VB_INVALID_BLK) {
        ret = hd_common_mem_release_block(*blk);
        if (ret != HD_OK) {
            printf("Pseudo %s release failed=%d\n", name, ret);
            return ret;
        }
    }
    *blk = 0;
    *pa = 0;
    return HD_OK;
}

static HD_RESULT pseudo_uninit_locked(void)
{
    HD_RESULT ret, output_ret;
    if (pseudo_ive_inited) {
        ret = vendor_ive_uninit();
        if (ret != HD_OK) return ret;
        pseudo_ive_inited = 0;
    }
    ret = pseudo_release_buffer(&pseudo_buffer->Y_plane_va,
        &pseudo_buffer->Y_plane_pa, &pseudo_buffer->Y_plane_blk, 256, "LUT");
    output_ret = pseudo_release_buffer(&pseudo_buffer->Y_out_va,
        &pseudo_buffer->Y_out_pa, &pseudo_buffer->Y_out_blk,
        pseudo_buffer->Y_out_size, "Y output");
    if (output_ret == HD_OK) pseudo_buffer->Y_out_size = 0;
    return ret != HD_OK ? ret : output_ret;
}

/* Follow init_thermal_frame: common pool -> PA -> VA -> clear/cache. */
static HD_RESULT pseudo_alloc_buffer(HD_COMMON_MEM_VB_BLK *blk,
    UINTPTR *pa, UINTPTR *va, UINT32 size, const char *name)
{
    HD_RESULT ret;
    *blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, size, DDR_ID0);
    if (*blk == 0 || *blk == HD_COMMON_MEM_VB_INVALID_BLK) {
        *blk = 0;
        printf("Pseudo %s get block failed (size=%u)\n", name, (unsigned)size);
        return HD_ERR_NG;
    }
    *pa = hd_common_mem_blk2pa(*blk);
    if (*pa == 0) {
        printf("Pseudo %s blk2pa failed\n", name);
        return HD_ERR_NG;
    }
    *va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, *pa, size);
    if (*va == 0 || *va == (UINTPTR)-1) {
        *va = 0;
        printf("Pseudo %s mmap failed\n", name);
        return HD_ERR_NG;
    }
    memset((void *)*va, 0, size);
    ret = hd_common_mem_flush_cache((void *)*va, size);
    if (ret != HD_OK) printf("Pseudo %s flush failed=%d\n", name, ret);
    return ret;
}

static HD_RESULT pseudo_init_locked(int Y_size)
{
    HD_RESULT ret, cleanup_ret;

    if (pseudo_type == PSEUDO_TYPE_NORMAL) return HD_OK;
    if (Y_size <= 0 || pseudo_ive_inited) return HD_ERR_NG;
    pseudo_buffer->Y_out_size = Y_size;
    ret = pseudo_alloc_buffer(&pseudo_buffer->Y_plane_blk,
        &pseudo_buffer->Y_plane_pa, &pseudo_buffer->Y_plane_va, 256, "LUT");
    if (ret != HD_OK) goto fail;
    ret = pseudo_alloc_buffer(&pseudo_buffer->Y_out_blk,
        &pseudo_buffer->Y_out_pa, &pseudo_buffer->Y_out_va, Y_size, "Y output");
    if (ret != HD_OK) goto fail;

    Pseudo_LUT_init();
    ret = hd_common_mem_flush_cache((void *)pseudo_buffer->Y_plane_va, 256);
    if (ret != HD_OK) goto fail;
    ret = vendor_ive_init();
    if (ret != HD_OK) {
        printf("Pseudo vendor_ive_init failed=%d\n", ret);
        goto fail;
    }
    pseudo_ive_inited = 1;
    return HD_OK;
fail:
    cleanup_ret = pseudo_uninit_locked();
    if (cleanup_ret != HD_OK) printf("Pseudo cleanup failed=%d\n", cleanup_ret);
    return ret;
}

void Pseudo_LUT_init(void)
{
    const UINT8 (*pseudo_rgb)[3];

    UINT32 i;
    UINT32 segment;
    UINT32 pos;
    UINT32 base;
    UINT32 next;

    INT32 r;
    INT32 g;
    INT32 b;
    INT32 y;
    INT32 u;
    INT32 v;

    if (pseudo_type == PSEUDO_TYPE_NORMAL || !pseudo_buffer->Y_plane_va)
        return;
    pseudo_rgb = pseudo_palettes[pseudo_type - 1];

    for (i = 0; i < 256U; i++) {

        /*
         * 32 control points = 31 segments
         */
        segment = (i * 31U) / 255U;

        if (segment >= 31U) {
            segment = 30U;
            pos = 255U;
        } else {
            pos = (i * 31U) % 255U;
        }

        base = segment;
        next = segment + 1U;

        /*
         * RGB interpolation
         */
        r = (INT32)pseudo_rgb[base][0] +
            ((INT32)pseudo_rgb[next][0] -
             (INT32)pseudo_rgb[base][0]) *
             (INT32)pos / 255;

        g = (INT32)pseudo_rgb[base][1] +
            ((INT32)pseudo_rgb[next][1] -
             (INT32)pseudo_rgb[base][1]) *
             (INT32)pos / 255;

        b = (INT32)pseudo_rgb[base][2] +
            ((INT32)pseudo_rgb[next][2] -
             (INT32)pseudo_rgb[base][2]) *
             (INT32)pos / 255;


        /*
         * RGB -> YUV
         * BT.601 Limited Range
         */
        y = 16 +
            (66 * r + 129 * g + 25 * b + 128) / 256;

        u = 128 +
            (-38 * r - 74 * g + 112 * b + 128) / 256;

        v = 128 +
            (112 * r - 94 * g - 18 * b + 128) / 256;


        /*
         * Clamp
         */
        if (y < 16)
            y = 16;
        else if (y > 235)
            y = 235;

        if (u < 16)
            u = 16;
        else if (u > 240)
            u = 240;

        if (v < 16)
            v = 16;
        else if (v > 240)
            v = 240;


        // Save LUT
        ((UINT8 *)pseudo_buffer->Y_plane_va)[i] = (UINT8)y;

        pseudo_u[i] = (UINT8)u;
        pseudo_v[i] = (UINT8)v;
    }
	
	// /* 刷新 CPU 寫入的 LUT 數據 */
    hd_common_mem_flush_cache(
        (void *)pseudo_buffer->Y_plane_va,
        256
    );
}


static int pseudo_process_locked(const HD_VIDEO_FRAME *p_video_frame)
{
	UINTPTR y_pa;
	UINTPTR uv_pa;
	UINTPTR y_va;
	UINTPTR uv_va;
	UINTPTR y_map_pa;
	UINTPTR uv_map_pa;
	UINTPTR y_map_va;
	UINTPTR uv_map_va;

	UINT32 width;
	UINT32 height;

	UINT32 y_stride;
	UINT32 uv_stride;
	UINT32 y_size;
	UINT32 uv_size;
	UINT32 y_map_size;
	UINT32 uv_map_size;
	UINT32 y_data_offset;
	UINT32 uv_data_offset;

    /* p_video_frame->phy_addr[0] 和 ive_map 設定 */
    UINTPTR block_pa;
    IVE_SRC_IMAGE_S src = {0}, lut = {0};
    IVE_DST_IMAGE_S dst = {0};
    UINT32 handle = 0;
    INT32 ive_ret;
    int result = -1;

	UINT32 x;
	UINT32 y;

	UINT32 color;
	// UINT32 sum;

	HD_RESULT ret;

	const UINT8 *row0;
	// const UINT8 *row1;

	UINT8 *uv_row;

	/*
	 * Check frame
	 */
	if (pseudo_type == PSEUDO_TYPE_NORMAL) return 0;
	if (!pseudo_ive_inited || p_video_frame == NULL)
		return -1;

	if (p_video_frame->phy_addr[0] == 0)
		return -1;

	if (p_video_frame->phy_addr[1] == 0)
		return -1;

	if (p_video_frame->pxlfmt != HD_VIDEO_PXLFMT_YUV420) return -1;
	width = p_video_frame->dim.w;
	height = p_video_frame->dim.h;

	if (width < IVE_MAP_WIDTH_MIN || width > IVE_MAP_WIDTH_MAX ||
	    height < IVE_MAP_HEIGHT_MIN || height > IVE_MAP_HEIGHT_MAX)
		return -1;

	if ((width & 1U) != 0 || (height & 1U) != 0)
		return -1;

	if (pseudo_buffer->Y_out_size <= 0 ||
	    width > (UINT32)pseudo_buffer->Y_out_size / height)
		return -1;

	// Y Plane
	y_pa = p_video_frame->phy_addr[0];
	y_stride = p_video_frame->loff[0];

	if (y_stride < width)
		y_stride = width;

	/* 在計算 Y 平面分配大小之前檢查乘法溢位. */
	if (y_stride > UINT32_MAX / height) return -1;
	y_size = y_stride * height;

	// Get block physical address
	block_pa = (p_video_frame->blk != 0 &&
	            p_video_frame->blk != HD_COMMON_MEM_VB_INVALID_BLK) ?
		(UINTPTR)hd_common_mem_blk2pa(p_video_frame->blk) : 0;

	// Y mmap address
	if (block_pa != 0 && y_pa >= block_pa) {
		y_map_pa = block_pa;
		y_data_offset =
			(UINT32)(y_pa - block_pa);
	} else {
		y_map_pa = y_pa;
		y_data_offset = 0;
	}

	y_map_size = y_data_offset + y_size;
	y_map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, y_map_pa, y_map_size);

	if (y_map_va == 0 || y_map_va == (UINTPTR)-1)
		return -1;
	y_va = y_map_va + y_data_offset;

	// UV Plane
	uv_pa = p_video_frame->phy_addr[1];
	uv_stride = p_video_frame->loff[1];
	if (uv_stride < width)
		uv_stride = width;

	uv_size = uv_stride * (height / 2U);

	// UV mmap address
	if (block_pa != 0 && uv_pa >= block_pa) {
		uv_map_pa = block_pa;
		uv_data_offset = (UINT32)(uv_pa - block_pa);
	} else {
		uv_map_pa = uv_pa;
		uv_data_offset = 0;
	}

	/* 檢查 UV 平面尺寸溢出，並在失敗時釋放現有的 Y 軸映射 */
	if (uv_pa - uv_map_pa > UINT32_MAX - uv_size) {
		hd_common_mem_munmap((void *)y_map_va, y_map_size);
		return -1;
	}
	uv_map_size = uv_data_offset + uv_size;

	uv_map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, uv_map_pa, uv_map_size);

	if (uv_map_va == 0 || uv_map_va == (UINTPTR)-1) {
		hd_common_mem_munmap((void *)y_map_va,	y_map_size);
		return -1;
	}

	uv_va = uv_map_va + uv_data_offset;

	/*
	 * Flush cache before accessing image
	 */
	hd_common_mem_flush_cache((void *)y_map_va, y_map_size);
	hd_common_mem_flush_cache((void *)uv_map_va, uv_map_size);

	// Generate UV  (average 4 Y)
	// for (y = 0; y < height; y += 2U) {
	// 	row0 = (const UINT8 *)(y_va + y * y_stride);
	// 	row1 = (const UINT8 *)(y_va + (y + 1U) * y_stride);
	// 	uv_row = (UINT8 *)(uv_va + (y / 2U) * uv_stride);

	// 	for (x = 0; x < width; x += 2U) {
	// 		// Average original 2x2 grayscale pixels
	// 		sum =(UINT32)row0[x] + (UINT32)row0[x + 1U] + (UINT32)row1[x] + (UINT32)row1[x + 1U];
	// 		color = sum / 4U;

	// 		uv_row[x]     = pseudo_u[color];
	// 		uv_row[x + 1U] = pseudo_v[color];
	// 	}
	// }

	// Generate UV  (only the top-left Y)
	for (y = 0; y < height; y += 2U) {
		row0 = (const UINT8 *)(y_va + y * y_stride);
		uv_row = (UINT8 *)(uv_va + (y / 2U) * uv_stride);

		for (x = 0; x < width; x += 2U) {

			// Use only the top-left Y pixel of each 2x2 block
			color = row0[x];

			uv_row[x]      = pseudo_u[color];
			uv_row[x + 1U] = pseudo_v[color];
		}
	}


    src.enDataType = LIB_IVE_DATA_TYPE_U8C1;
    src.u64Pa = y_pa;
    src.u64Va = y_va;
    src.u32Width = width;
    src.u32Height = height;
    src.u32Stride = y_stride;
    lut.enDataType = LIB_IVE_DATA_TYPE_U8C1;
    lut.u64Pa = pseudo_buffer->Y_plane_pa;
    lut.u64Va = pseudo_buffer->Y_plane_va;
    lut.u32Width = 256;
    lut.u32Height = 1;
    lut.u32Stride = 256;
    dst.enDataType = LIB_IVE_DATA_TYPE_U8C1;
    dst.u64Pa = pseudo_buffer->Y_out_pa;
    dst.u64Va = pseudo_buffer->Y_out_va;
    dst.u32Width = width;
    dst.u32Height = height;
    dst.u32Stride = width;

    ive_ret = NVT_IVE_Map(&handle, &src, &lut, &dst, 1);
    if (ive_ret != LIB_IVE_OK) {
        printf("NVT_IVE_Map failed=%d\n", (int)ive_ret);
        goto cleanup;
    }
	
	hd_common_mem_flush_cache((void *)pseudo_buffer->Y_out_va, pseudo_buffer->Y_out_size);
	for (y = 0; y < height; ++y) {
		memcpy((void *)(y_va + y * y_stride),
		       (const void *)(pseudo_buffer->Y_out_va + y * width),
		       width);
	}
	
   
    // Flush Y cache
	hd_common_mem_flush_cache( (void *)y_map_va, y_map_size);
	// Flush UV cache
	hd_common_mem_flush_cache( (void *)uv_map_va, uv_map_size);


	
    result = 0;

cleanup:
    ret = hd_common_mem_munmap((void *)uv_map_va, uv_map_size);
    if (ret != HD_OK) result = -1;
    ret = hd_common_mem_munmap((void *)y_map_va, y_map_size);
    if (ret != HD_OK) result = -1;
    return result;
}


/* Serialize lifecycle and LUT changes with complete frame processing. */
HD_RESULT Pseudo_Init(int Y_size)
{
    HD_RESULT ret;
    pthread_mutex_lock(&pseudo_lock);
    pseudo_frame_size = Y_size;
    ret = pseudo_init_locked(Y_size);
    pthread_mutex_unlock(&pseudo_lock);
    return ret;
}

HD_RESULT Pseudo_Uninit(void)
{
    HD_RESULT ret;
    pthread_mutex_lock(&pseudo_lock);
    ret = pseudo_uninit_locked();
    pthread_mutex_unlock(&pseudo_lock);
    return ret;
}

int PseudoColor_YUV420(const HD_VIDEO_FRAME *frame)
{
    int ret;
    pthread_mutex_lock(&pseudo_lock);
    if (pseudo_requested_mode != (int)pseudo_type) {
        PSEUDO_TYPE old_mode = pseudo_type;
        HD_RESULT status = HD_OK;
        pseudo_type = (PSEUDO_TYPE)pseudo_requested_mode;
        if (pseudo_type != PSEUDO_TYPE_NORMAL) {
            if (!pseudo_ive_inited) {
                status = pseudo_init_locked(pseudo_frame_size);
            } else {
                Pseudo_LUT_init();
                status = hd_common_mem_flush_cache(
                    (void *)pseudo_buffer->Y_plane_va, 256);
            }
        }
        if (status != HD_OK) {
            pseudo_type = old_mode;
            pseudo_requested_mode = (int)old_mode;
            if (pseudo_ive_inited && old_mode != PSEUDO_TYPE_NORMAL) {
                Pseudo_LUT_init();
                if (hd_common_mem_flush_cache(
                        (void *)pseudo_buffer->Y_plane_va, 256) != HD_OK) {
                    /* Bypass if the restored hardware LUT cannot be synchronized. */
                    pseudo_type = PSEUDO_TYPE_NORMAL;
                    pseudo_requested_mode = PSEUDO_TYPE_NORMAL;
                }
            }
            printf("Pseudo color switch failed; CFG saved, runtime=%s\n",
                   pseudo_mode_name((int)pseudo_type));
            pthread_mutex_unlock(&pseudo_lock);
            return -1;
        }
        printf("Pseudo color applied: %s\n", pseudo_mode_name((int)pseudo_type));
    }
    ret = pseudo_process_locked(frame);
    pthread_mutex_unlock(&pseudo_lock);
    return ret;
}
