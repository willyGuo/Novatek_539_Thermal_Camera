#ifndef VPE_MIRROR_H
#define VPE_MIRROR_H
#include "hdal.h"
HD_RESULT vpe_mirror_start(UINT32 width, UINT32 height, UINT32 buffer_size);
HD_RESULT vpe_mirror_stop(void);
HD_RESULT vpe_mirror_process(HD_VIDEO_FRAME *input, HD_VIDEO_FRAME *output);
HD_RESULT vpe_mirror_release(HD_VIDEO_FRAME *output);
int vpe_mirror_input_pending(void);
#endif
