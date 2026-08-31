#ifndef NUC_H
#define NUC_H

#include "hdal.h"

#define NUC_GAIN_SHIFT 8
#define NUC_GAIN_ONE (1U << NUC_GAIN_SHIFT)

typedef int (*NUC_CAPTURE_RAW_CB)(UINTPTR dst_va, UINT32 size);
typedef void (*NUC_CALIBRATION_STATE_CB)(const char *name, int active);

typedef struct {
	UINT32 raw_width;
	UINT32 raw_height;
	UINT32 active_x;
	UINT32 active_y;
	UINT32 active_width;
	UINT32 active_height;
} NUC_IMAGE_LAYOUT;

int nuc_menu_run(void);
void nuc_bind_runtime(UINTPTR k_va, UINTPTR b_va, UINT32 table_size,
		      const NUC_IMAGE_LAYOUT *layout);
void nuc_bind_capture_callback(NUC_CAPTURE_RAW_CB capture_raw);
void nuc_bind_calibration_state_callback(
	NUC_CALIBRATION_STATE_CB calibration_state);
int nuc_init_table(void);
int nuc_reload_table(void);
int nuc_update_sensor_temp_table(void);
int nuc_update_sensor_temp_table_periodic(void);
int nuc_five_point_sensor_temp_calibration(void);
int nuc_five_point_blackbody_temp_calibration(void);
int nuc_set_blackbody_dynamic_enabled(int enable);
int nuc_is_blackbody_dynamic_enabled(void);
int nuc_prepare_blackbody_dynamic_table(HD_VIDEO_FRAME *in_frame);
void nuc_set_sensor_temp_point(UINT32 point, float temp_c);
float nuc_get_sensor_temp_point(UINT32 point);
void nuc_set_enabled(int enable);
int nuc_is_enabled(void);
void nuc_set_sensor_temp_interval_sec(UINT32 interval_sec);
UINT32 nuc_get_sensor_temp_interval_sec(void);

HD_RESULT nuc_apply(HD_VIDEO_FRAME *in_frame,
		    HD_VIDEO_FRAME *out_frame,
		    UINTPTR k_frame_pa,
		    UINTPTR b_frame_pa,
		    UINTPTR out_pa,
		    HD_COMMON_MEM_VB_BLK out_blk,
		    UINT32 raw_width,
		    UINT32 raw_height);

#endif
