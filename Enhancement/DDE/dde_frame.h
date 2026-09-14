#ifndef TIFFANY_DDE_FRAME_H
#define TIFFANY_DDE_FRAME_H
#include "hdal.h"
int dde_process_frame(const HD_VIDEO_FRAME *frame);
void dde_frame_release(void);
#endif
