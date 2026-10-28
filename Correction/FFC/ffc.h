#ifndef FFC_H
#define FFC_H

#include "hdal.h"

/* FFC unity gain and GFX shift must use the same format, independent of NUC. */
#define FFC_GAIN_SHIFT 8U
#define FFC_GAIN_ONE (1U << FFC_GAIN_SHIFT)

typedef int (*FFC_TEST_CB)(void);

int ffc_menu_run(void);
void ffc_bind_test_callback(FFC_TEST_CB run_test);
void ffc_bind_shading_callback(FFC_TEST_CB calibrate);
void ffc_set_shading_enabled(int enabled);
int ffc_is_shading_enabled(void);
int ffc_shading_is_ready(void);
int ffc_shading_is_active(void);
void ffc_set_shading_input_mode(int nuc_enabled, int tdc_enabled, int dynamic_nuc);
UINT32 ffc_shading_generation(void);
/* Packed active-area, NUC-corrected and pre-FFC averaged RAWs. */
int ffc_shading_calibrate(const UINT16 *open_raw, const UINT16 *shutter_raw,
    int tdc_enabled);
void ffc_bind_runtime(UINTPTR b_va, UINT32 table_size);
int ffc_init_table(void);
int ffc_update_table_from_raw(UINTPTR raw_va,
			      UINT32 lineoffset,
			      UINT32 active_x,
			      UINT32 active_y,
			      UINT32 active_w,
			      UINT32 active_h);
void ffc_set_enabled(int enable);
int ffc_is_enabled(void);
void ffc_set_startup_enabled(int enable);
int ffc_is_startup_enabled(void);
void ffc_set_shutter_interval_sec(UINT32 interval_sec);
UINT32 ffc_get_shutter_interval_sec(void);
int ffc_table_is_ready(void);
int ffc_compare_gfx_samples(UINTPTR input_va,
			    UINTPTR k_va,
			    UINTPTR output_va,
			    UINT32 lineoffset,
			    UINT32 active_x,
			    UINT32 active_y,
			    UINT32 active_w,
			    UINT32 active_h,
			    UINT32 sample_count);

/* Reference GFX sequence: the final shift pass writes its result to sum_pa. */
HD_RESULT ffc_accumulate_frame(UINTPTR raw_pa,
			       UINTPTR sum_pa,
			       UINTPTR out_pa,
			       UINTPTR zero_pa,
			       UINT32 raw_width,
			       UINT32 raw_height,
			       UINT32 shift);

HD_RESULT ffc_apply(HD_VIDEO_FRAME *in_frame,
		    HD_VIDEO_FRAME *out_frame,
		    UINTPTR k_frame_pa,
		    UINTPTR b_frame_pa,
		    UINTPTR out_pa,
		    HD_COMMON_MEM_VB_BLK out_blk,
		    UINT32 raw_width,
		    UINT32 raw_height);

#endif
