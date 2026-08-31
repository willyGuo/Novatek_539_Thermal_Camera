#include "video_output.h"

#include <stdio.h>
#include <string.h>

int video_output_mode_is_valid(unsigned int mode)
{
	return mode == VIDEO_OUTPUT_MODE_UVC ||
	       mode == VIDEO_OUTPUT_MODE_RTSP;
}

const char *video_output_mode_name(VIDEO_OUTPUT_MODE mode)
{
	switch (mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		return "UVC";
	case VIDEO_OUTPUT_MODE_RTSP:
		return "RTSP";
	default:
		return "unknown";
	}
}

HD_RESULT video_output_module_init(void)
{
	return hd_videoenc_init();
}

HD_RESULT video_output_module_uninit(void)
{
	return hd_videoenc_uninit();
}

HD_RESULT video_output_open(VIDEO_OUTPUT *output, VIDEO_OUTPUT_MODE mode,
	UINT32 width, UINT32 height, UINT32 fps, UINT32 rtsp_bitrate,
	UINT32 isp_id)
{
	HD_RESULT ret;

	if (output == NULL || !video_output_mode_is_valid((unsigned int)mode)) {
		return HD_ERR_NG;
	}

	memset(output, 0, sizeof(*output));
	output->mode = mode;
	printf("Selected video output: %s\n", video_output_mode_name(mode));

	switch (mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		ret = uvc_output_open(&output->backend.uvc,
			width, height, fps, isp_id);
		break;
	case VIDEO_OUTPUT_MODE_RTSP:
		ret = rtsp_output_open(&output->backend.rtsp,
			width, height, fps, rtsp_bitrate, isp_id);
		break;
	default:
		return HD_ERR_NG;
	}

	if (ret == HD_OK) {
		output->opened = 1;
	}
	return ret;
}

HD_RESULT video_output_start(VIDEO_OUTPUT *output)
{
	if (output == NULL || !output->opened) {
		return HD_ERR_NG;
	}

	switch (output->mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		return uvc_output_start(&output->backend.uvc);
	case VIDEO_OUTPUT_MODE_RTSP:
		return rtsp_output_start(&output->backend.rtsp);
	default:
		return HD_ERR_NG;
	}
}

HD_RESULT video_output_stop(VIDEO_OUTPUT *output)
{
	if (output == NULL) {
		return HD_ERR_NG;
	}
	if (!output->opened) {
		return HD_OK;
	}

	switch (output->mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		return uvc_output_stop(&output->backend.uvc);
	case VIDEO_OUTPUT_MODE_RTSP:
		return rtsp_output_stop(&output->backend.rtsp);
	default:
		return HD_ERR_NG;
	}
}

HD_RESULT video_output_close(VIDEO_OUTPUT *output)
{
	HD_RESULT ret;

	if (output == NULL) {
		return HD_ERR_NG;
	}
	if (!output->opened) {
		memset(output, 0, sizeof(*output));
		return HD_OK;
	}

	switch (output->mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		ret = uvc_output_close(&output->backend.uvc);
		break;
	case VIDEO_OUTPUT_MODE_RTSP:
		ret = rtsp_output_close(&output->backend.rtsp);
		break;
	default:
		ret = HD_ERR_NG;
		break;
	}

	memset(output, 0, sizeof(*output));
	return ret;
}

HD_RESULT video_output_request_encoded_snapshot(VIDEO_OUTPUT *output,
	const char *file_path)
{
	if (output == NULL || !output->opened || file_path == NULL) {
		return HD_ERR_NG;
	}

	switch (output->mode) {
	case VIDEO_OUTPUT_MODE_UVC:
		return uvc_output_request_mjpeg_snapshot(&output->backend.uvc,
			file_path);
	case VIDEO_OUTPUT_MODE_RTSP:
		printf("Encoded JPEG diagnostic is unavailable in RTSP mode\n");
		return HD_ERR_NG;
	default:
		return HD_ERR_NG;
	}
}
