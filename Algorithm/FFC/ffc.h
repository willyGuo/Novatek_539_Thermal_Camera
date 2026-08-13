#ifndef FFC_H
#define FFC_H

#include "hdal.h"

int ffc_menu_run(void);
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
void ffc_set_shutter_interval_sec(UINT32 interval_sec);
UINT32 ffc_get_shutter_interval_sec(void);
int ffc_table_is_ready(void);

HD_RESULT ffc_accumulate_frame(UINTPTR raw_pa,
			       UINTPTR sum_pa,
			       UINTPTR out_pa,
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
