#ifndef TDC_H
#define TDC_H

#include "hdal.h"

typedef int (*TDC_CAPTURE_RAW_CB)(UINTPTR dst_va, UINT32 size);

typedef struct {
	UINT32 raw_width;
	UINT32 raw_height;
	UINT32 active_x;
	UINT32 active_y;
	UINT32 active_width;
	UINT32 active_height;
} TDC_IMAGE_LAYOUT;

HD_RESULT init_thermal_TDC_mem(void);
HD_RESULT tdc_update_correction_table(void);
HD_RESULT tdc_apply(HD_VIDEO_FRAME *in_frame,
		    HD_VIDEO_FRAME *out_frame,
		    UINTPTR out_pa,
		    HD_COMMON_MEM_VB_BLK out_blk,
		    UINT32 raw_width,
		    UINT32 raw_height);
HD_RESULT exit_TDC_mem(void);
void tdc_bind_runtime(const TDC_IMAGE_LAYOUT *layout);
void tdc_bind_capture_callback(TDC_CAPTURE_RAW_CB capture_raw);
void tdc_set_enabled(int enable);
int tdc_is_enabled(void);
UINT32 tdc_correction_generation(void);
void tdc_set_interval_sec(UINT32 interval_sec);
UINT32 tdc_get_interval_sec(void);
int tdc_menu_run(void);

#endif /* TDC_H */
