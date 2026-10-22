#ifndef VIDEO_OUTPUT_H
#define VIDEO_OUTPUT_H

#include "hdal.h"

#include "../RTSP/rtsp_output.h"
#include "../UVC/uvc_output.h"

typedef enum {
	VIDEO_OUTPUT_MODE_UVC = 0,
	VIDEO_OUTPUT_MODE_RTSP = 1,
} VIDEO_OUTPUT_MODE;

typedef struct {
	VIDEO_OUTPUT_MODE mode;
	int opened;
	FRAME_TRACE encoder_input_trace;
	union {
		UVC_OUTPUT uvc;
		RTSP_OUTPUT rtsp;
	} backend;
} VIDEO_OUTPUT;

int video_output_mode_is_valid(unsigned int mode);
const char *video_output_mode_name(VIDEO_OUTPUT_MODE mode);

HD_RESULT video_output_module_init(void);
HD_RESULT video_output_module_uninit(void);
HD_RESULT video_output_open(VIDEO_OUTPUT *output, VIDEO_OUTPUT_MODE mode,
	UINT32 width, UINT32 height, UINT32 fps, UINT32 rtsp_bitrate,
	UINT32 isp_id);
HD_RESULT video_output_start(VIDEO_OUTPUT *output);
HD_RESULT video_output_stop(VIDEO_OUTPUT *output);
HD_RESULT video_output_close(VIDEO_OUTPUT *output);
HD_RESULT video_output_push_frame(VIDEO_OUTPUT *output,
	HD_VIDEO_FRAME *frame);
HD_RESULT video_output_request_encoded_snapshot(VIDEO_OUTPUT *output,
	const char *file_path);

#endif
