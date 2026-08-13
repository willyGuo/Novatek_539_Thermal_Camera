#ifndef RTSP_OUTPUT_H
#define RTSP_OUTPUT_H

#include "hdal.h"

typedef struct {
	HD_PATH_ID enc_path;
	int encoder_started;
	int service_started;
	int service_shmid;
	int service_shm_created;
	void *service_shm;
} RTSP_OUTPUT;

HD_RESULT rtsp_output_open(RTSP_OUTPUT *output, UINT32 width, UINT32 height,
	UINT32 fps, UINT32 bitrate, UINT32 isp_id);
HD_RESULT rtsp_output_start(RTSP_OUTPUT *output);
HD_RESULT rtsp_output_stop(RTSP_OUTPUT *output);
HD_RESULT rtsp_output_close(RTSP_OUTPUT *output);

#endif
