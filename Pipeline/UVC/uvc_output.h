#ifndef UVC_OUTPUT_H
#define UVC_OUTPUT_H

#include <pthread.h>
#include <stdint.h>

#include "hdal.h"
#include "../frame_trace.h"

typedef struct {
	HD_PATH_ID enc_path;
	UINT32 width;
	UINT32 height;
	UINT32 fps;
	pthread_t thread_id;
	volatile int thread_exit;
	volatile int host_streaming;
	volatile UINT32 host_format;
	int thread_started;
	int encoder_started;
	int uvac_opened;
	uintptr_t uvac_pa;
	uintptr_t uvac_va;
	UINT32 uvac_size;
	uintptr_t frame_pa;
	uintptr_t frame_va;
	UINT32 frame_size;
	int uvac_frame_pending;
	int bitstream_held;
	HD_VIDEOENC_BS held_bitstream;
	unsigned long long pulled, released, release_errors, wait_errors;
	uintptr_t encoder_va;
	UINT32 encoder_map_size;
	volatile int failed;
	FRAME_TRACE encoder_output_trace, usb_submit_trace, usb_done_trace;
} UVC_OUTPUT;

HD_RESULT uvc_output_open(UVC_OUTPUT *output, UINT32 width, UINT32 height,
	UINT32 fps, UINT32 isp_id);
HD_RESULT uvc_output_start(UVC_OUTPUT *output);
HD_RESULT uvc_output_stop(UVC_OUTPUT *output);
HD_RESULT uvc_output_close(UVC_OUTPUT *output);
HD_RESULT uvc_output_request_mjpeg_snapshot(UVC_OUTPUT *output,
	const char *file_path);

#endif
