#ifndef DEVICE_FOS_H
#define DEVICE_FOS_H

#include "hdal.h"

#define FOS_RAW14_TARGET_DEFAULT 8192U

typedef int (*FOS_CAPTURE_RAW_CB)(UINTPTR dst_va, UINT32 size);
typedef void (*FOS_CALIBRATION_STATE_CB)(const char *name, int active);

void fos_bind_capture_callback(FOS_CAPTURE_RAW_CB capture_raw,
			       UINT32 raw_width, UINT32 raw_height);
void fos_bind_calibration_state_callback(
	FOS_CALIBRATION_STATE_CB calibration_state);
int fos_adjust_once(void);
UINT16 fos_get_target_raw14(void);
int fos_menu_run(void);

#endif /* DEVICE_FOS_H */
