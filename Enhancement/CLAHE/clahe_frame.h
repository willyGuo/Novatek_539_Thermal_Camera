#ifndef TIFFANY_CLAHE_FRAME_H
#define TIFFANY_CLAHE_FRAME_H
#include "hdal.h"
int clahe_process_frame(const HD_VIDEO_FRAME *frame);
void clahe_frame_release(void);
#endif
