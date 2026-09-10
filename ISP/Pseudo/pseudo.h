#ifndef PSEUDO_H
#define PSEUDO_H

#include "hdal.h"

/* Display modes: normal grayscale plus three palettes. */
typedef enum {
    PSEUDO_TYPE_NORMAL = 0,
    PSEUDO_TYPE_REVERSE,
    PSEUDO_TYPE_RAINBOW,
    PSEUDO_TYPE_HOT_IRON
} PSEUDO_TYPE;

int pseudo_set_mode(int mode);
int pseudo_get_mode(void);
/* Queue a mode change for the next display frame. */
int pseudo_request_mode(int mode);
const char *pseudo_mode_name(int mode);

int PseudoColor_YUV420(const HD_VIDEO_FRAME *p_video_frame);
HD_RESULT Pseudo_Init(int Y_size);
HD_RESULT Pseudo_Uninit(void);
void Pseudo_LUT_init(void);

#endif /* PSEUDO_H */
