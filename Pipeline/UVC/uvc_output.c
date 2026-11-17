#include "uvc_output.h"
#include "../../Config/hardware_config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#if defined(__LINUX)
#include <unistd.h>
#endif

#include "UVAC.h"
#include "kwrap/error_no.h"

#define UVC_USB_VID                 0x0603U
#define UVC_USB_PID                 0x8612U
#define UVC_FRAME_TIMEOUT_MS        100
#define UVC_AUDIO_SAMPLE_RATE       48000U
#define UVC_MJPEG_BITRATE_BPS       (12U * 1024U * 1024U)
#define UVC_MJPEG_ENCODER_BUF_MS    3000U
#define UVC_MJPEG_QUALITY           100U
#define UVC_MJPEG_MAX_FRAME_SIZE    (2U * 1024U * 1024U)
#define UVC_DIAGNOSTIC_PATH_SIZE    192U

/* UVAC callbacks do not carry an application pointer. Only one UVC output is used. */
static UVC_OUTPUT *g_uvc_output;
static pthread_mutex_t g_uvc_diagnostic_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_uvc_mjpeg_snapshot_pending;
static char g_uvc_mjpeg_snapshot_path[UVC_DIAGNOSTIC_PATH_SIZE];

static UVAC_VID_RESO g_uvc_mjpg_resolution[] = {
	{THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT, 1, 30, 0, 0},
};

/* This UVAC release builds H.264 and MJPEG descriptors as one group. */
static UVAC_VID_RESO g_uvc_h264_resolution[] = {
	{THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT, 1, 30, 0, 0},
};

static UINT32 g_uvc_audio_sample_rates[] = {
	UVC_AUDIO_SAMPLE_RATE,
};

static UINT16 g_uvc_manufacturer[] __attribute__((aligned(4))) = {
	0x0310, 'N', 'O', 'V', 'A', 'T', 'E', 'K'
};

static UINT16 g_uvc_product[] __attribute__((aligned(4))) = {
	0x0320, 'T', 'i', 'f', 'f', 'a', 'n', 'y', ' ',
	'T', 'h', 'e', 'r', 'm', 'a', 'l'
};

static UINT16 g_uvc_serial[] __attribute__((aligned(4))) = {
	0x0320, '5', '1', '0', '5', '5', '0', '0', '0',
	'0', '0', '0', '0', '1', '0', '0'
};

static UINT32 uvc_start_video_cb(UVAC_VID_DEV_CNT vid_dev_idx,
	UVAC_STRM_INFO *stream_info)
{
	UVC_OUTPUT *output = g_uvc_output;

	(void)vid_dev_idx;
	if (output == NULL || stream_info == NULL) {
		return 1U;
	}

	printf("UVC host start: %ux%u %u fps format=%u\n",
		stream_info->strmWidth, stream_info->strmHeight,
		stream_info->strmFps, stream_info->strmCodec);
	if (stream_info->strmCodec != UVAC_VIDEO_FORMAT_MJPG ||
	    stream_info->strmWidth != output->width ||
	    stream_info->strmHeight != output->height ||
	    stream_info->strmFps != output->fps) {
		printf("Unsupported UVC request; expected MJPEG %ux%u %u fps\n",
			output->width, output->height, output->fps);
		return 1U;
	}

	output->host_format = stream_info->strmCodec;
	output->host_streaming = 1;
	return E_OK;
}

static UINT32 uvc_stop_video_cb(UINT32 is_closed)
{
	(void)is_closed;
	if (g_uvc_output != NULL) {
		g_uvc_output->host_streaming = 0;
		g_uvc_output->host_format = 0;
	}
	printf("UVC host stop\n");
	return E_OK;
}

/* UVAC_CHANNEL_1V1A is the single-video channel used by hd_uvc_cam. */
static UINT32 uvc_start_audio_cb(UVAC_AUD_DEV_CNT aud_dev_idx,
	UVAC_STRM_INFO *stream_info)
{
	(void)aud_dev_idx;
	(void)stream_info;
	return E_OK;
}

static UINT32 uvc_set_volume_cb(UINT32 volume)
{
	(void)volume;
	return E_OK;
}

static HD_RESULT uvc_encoder_configure(HD_PATH_ID enc_path,
	UINT32 width, UINT32 height, UINT32 fps, UINT32 isp_id)
{
	HD_VIDEOENC_PATH_CONFIG path_config = {0};
	HD_VIDEOENC_FUNC_CONFIG func_config = {0};
	HD_VIDEOENC_IN input = {0};
	HD_VIDEOENC_OUT output = {0};
	HD_RESULT ret;

	path_config.max_mem.codec_type = HD_CODEC_TYPE_JPEG;
	path_config.max_mem.max_dim.w = width;
	path_config.max_mem.max_dim.h = height;
	path_config.max_mem.bitrate = UVC_MJPEG_BITRATE_BPS;
	path_config.max_mem.enc_buf_ms = UVC_MJPEG_ENCODER_BUF_MS;
	path_config.max_mem.svc_layer = HD_SVC_DISABLE;
	path_config.max_mem.ltr = FALSE;
	path_config.max_mem.rotate = FALSE;
	path_config.max_mem.source_output = FALSE;
	path_config.isp_id = isp_id;
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_PATH_CONFIG,
		&path_config);
	if (ret != HD_OK) {
		printf("set UVC MJPEG path config failed=%d\n", ret);
		return ret;
	}

	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_FUNC_CONFIG,
		&func_config);
	if (ret != HD_OK) {
		printf("set UVC MJPEG function config failed=%d\n", ret);
		return ret;
	}

	input.dir = HD_VIDEO_DIR_NONE;
	input.pxl_fmt = HD_VIDEO_PXLFMT_YUV420;
	input.dim.w = width;
	input.dim.h = height;
	input.frc = HD_VIDEO_FRC_RATIO(1, 1);
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_IN, &input);
	if (ret != HD_OK) {
		printf("set UVC MJPEG input failed=%d\n", ret);
		return ret;
	}

	output.codec_type = HD_CODEC_TYPE_JPEG;
	output.jpeg.retstart_interval = 0;
	output.jpeg.image_quality = UVC_MJPEG_QUALITY;
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_OUT_ENC_PARAM,
		&output);
	if (ret != HD_OK) {
		printf("set UVC MJPEG output failed=%d\n", ret);
	}
	return ret;
}

static int uvc_mjpeg_snapshot_is_pending(void)
{
	int pending;

	pthread_mutex_lock(&g_uvc_diagnostic_lock);
	pending = g_uvc_mjpeg_snapshot_pending;
	pthread_mutex_unlock(&g_uvc_diagnostic_lock);
	return pending;
}

static int uvc_take_mjpeg_snapshot_path(char *file_path, size_t path_size)
{
	size_t copy_length;
	int pending = 0;

	if (file_path == NULL || path_size == 0) {
		return 0;
	}

	pthread_mutex_lock(&g_uvc_diagnostic_lock);
	if (g_uvc_mjpeg_snapshot_pending) {
		copy_length = strlen(g_uvc_mjpeg_snapshot_path);
		if (copy_length >= path_size) {
			copy_length = path_size - 1U;
		}
		memcpy(file_path, g_uvc_mjpeg_snapshot_path, copy_length);
		file_path[copy_length] = '\0';
		g_uvc_mjpeg_snapshot_pending = 0;
		g_uvc_mjpeg_snapshot_path[0] = '\0';
		pending = 1;
	}
	pthread_mutex_unlock(&g_uvc_diagnostic_lock);
	return pending;
}

static void uvc_cancel_mjpeg_snapshot(void)
{
	pthread_mutex_lock(&g_uvc_diagnostic_lock);
	g_uvc_mjpeg_snapshot_pending = 0;
	g_uvc_mjpeg_snapshot_path[0] = '\0';
	pthread_mutex_unlock(&g_uvc_diagnostic_lock);
}

static int uvc_save_mjpeg_snapshot(uintptr_t frame_va, UINT32 frame_size,
	UINT64 timestamp)
{
	char file_path[UVC_DIAGNOSTIC_PATH_SIZE];
	FILE *fp = NULL;
	size_t write_size;
	int ret = -1;

	if (!uvc_take_mjpeg_snapshot_path(file_path, sizeof(file_path))) {
		return 0;
	}

	fp = fopen(file_path, "wb");
	if (fp == NULL) {
		printf("UVC MJPEG diagnostic open %s fail: %s\n",
		       file_path, strerror(errno));
		goto exit;
	}
	write_size = fwrite((const void *)frame_va, 1, frame_size, fp);
	if (write_size != frame_size || ferror(fp)) {
		printf("UVC MJPEG diagnostic write %s fail, size=%u expected=%u\n",
		       file_path, (unsigned int)write_size,
		       (unsigned int)frame_size);
		goto exit;
	}
	if (fflush(fp) != 0) {
		printf("UVC MJPEG diagnostic flush %s fail\n", file_path);
		goto exit;
	}
#if defined(__LINUX)
	if (fsync(fileno(fp)) != 0) {
		printf("UVC MJPEG diagnostic sync %s fail: %s\n",
		       file_path, strerror(errno));
		goto exit;
	}
#endif
	if (fclose(fp) != 0) {
		fp = NULL;
		printf("UVC MJPEG diagnostic close %s fail\n", file_path);
		goto exit;
	}
	fp = NULL;
	printf("UVC MJPEG diagnostic saved: %s (%u bytes, timestamp=%llu)\n",
	       file_path, (unsigned int)frame_size,
	       (unsigned long long)timestamp);
	ret = 0;

exit:
	if (fp != NULL) {
		fclose(fp);
	}
	return ret;
}

static HD_RESULT uvc_copy_and_send_mjpeg(UVC_OUTPUT *output,
	const HD_VIDEOENC_BUFINFO *encoder_buffer, uintptr_t encoder_va,
	const HD_VIDEOENC_BS *bitstream, int send_to_host)
{
	UVAC_STRM_FRM stream_frame = {0};
	UINT32 total_size = 0;
	UINT32 i;
	ER wait_ret;
	HD_RESULT cache_ret;
	if (output->uvac_frame_pending) return HD_ERR_NG;

	if (bitstream->pack_num == 0) {
		return HD_ERR_NG;
	}

	for (i = 0; i < bitstream->pack_num; i++) {
		uintptr_t pack_pa = bitstream->video_pack[i].phy_addr;
		UINT32 pack_size = bitstream->video_pack[i].size;
		UINT32 pack_offset;

		if (pack_pa < encoder_buffer->buf_info.phy_addr ||
		    pack_size > encoder_buffer->buf_info.buf_size) {
			printf("UVC MJPEG invalid encoder pack %u\n", i);
			return HD_ERR_NG;
		}
		pack_offset = (UINT32)(pack_pa - encoder_buffer->buf_info.phy_addr);
		if (pack_size > output->frame_size ||
		    pack_offset > encoder_buffer->buf_info.buf_size - pack_size ||
		    total_size > output->frame_size - pack_size) {
			printf("UVC MJPEG frame exceeds buffer: pack=%u total=%u\n",
				pack_size, total_size);
			return HD_ERR_NG;
		}

		cache_ret = hd_common_mem_cache_sync((void *)(encoder_va + pack_offset),
			pack_size, HD_COMMON_MEM_DMA_FROM_DEVICE);
		if (cache_ret != HD_OK) return cache_ret;
		memcpy((void *)(output->frame_va + total_size),
			(const void *)(encoder_va + pack_offset), pack_size);
		total_size += pack_size;
	}

	cache_ret = hd_common_mem_flush_cache((void *)output->frame_va, total_size);
	if (cache_ret != HD_OK) return cache_ret;
	if (send_to_host) {
		stream_frame.path = UVAC_STRM_VID;
		stream_frame.addr = output->frame_pa;
		stream_frame.size = total_size;
		stream_frame.pStrmHdr = 0;
		stream_frame.strmHdrSize = 0;
		stream_frame.va = output->frame_va;
		stream_frame.timestamp = bitstream->timestamp;
		stream_frame.frame_type = UVAC_VIDEO_FRM_NORMAL;
		UVAC_SetEachStrmInfo(&stream_frame);
		frame_trace_sample(&output->usb_submit_trace, "USB_SUBMIT", 0, bitstream->timestamp);
		output->uvac_frame_pending = 1;
		wait_ret = UVAC_WaitStrmDone(UVAC_STRM_VID);
		if (wait_ret != E_OK) {
			output->wait_errors++;
			printf("UVC wait failed=%d; retain transfer buffer until UVAC close\n", wait_ret);
			return HD_ERR_NG;
		}
		output->uvac_frame_pending = 0;
		frame_trace_sample(&output->usb_done_trace, "USB_DONE", 0, bitstream->timestamp);
	}
	uvc_save_mjpeg_snapshot(output->frame_va, total_size,
		bitstream->timestamp);
	return HD_OK;
}

static void *uvc_output_thread(void *arg)
{
	UVC_OUTPUT *output = (UVC_OUTPUT *)arg;
	HD_VIDEOENC_BUFINFO encoder_buffer = {0};
	HD_VIDEOENC_BS bitstream = {0};
	uintptr_t encoder_va;
	HD_RESULT ret;
	HD_RESULT send_ret;

	ret = hd_videoenc_get(output->enc_path, HD_VIDEOENC_PARAM_BUFINFO,
		&encoder_buffer);
	if (ret != HD_OK) {
		printf("get UVC MJPEG buffer info failed=%d\n", ret);
		output->failed = 1;
		return NULL;
	}
	encoder_va = (uintptr_t)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, encoder_buffer.buf_info.phy_addr,
		encoder_buffer.buf_info.buf_size);
	if (encoder_va == 0) {
		printf("map UVC MJPEG buffer failed\n");
		output->failed = 1;
		return NULL;
	}

	output->encoder_va = encoder_va;
	output->encoder_map_size = encoder_buffer.buf_info.buf_size;
	while (!output->thread_exit) {
		ret = hd_videoenc_pull_out_buf(output->enc_path, &bitstream,
			UVC_FRAME_TIMEOUT_MS);
		if (ret != HD_OK) {
			if (ret != HD_ERR_UNDERRUN && ret != HD_ERR_TIMEDOUT) {
				printf("UVC encoder pull failed=%d\n", ret);
			}
			continue;
		}

		/* Always drain the encoder so it cannot fill before the host opens UVC. */
		output->held_bitstream = bitstream;
		output->bitstream_held = 1;
		output->pulled++;
		frame_trace_sample(&output->encoder_output_trace, "ENC_OUT", 0, bitstream.timestamp);
		send_ret = HD_OK;
		if ((output->host_streaming &&
		     output->host_format == UVAC_VIDEO_FORMAT_MJPG) ||
		    uvc_mjpeg_snapshot_is_pending()) {
			send_ret = uvc_copy_and_send_mjpeg(output, &encoder_buffer,
				encoder_va, &bitstream,
				output->host_streaming &&
				output->host_format == UVAC_VIDEO_FORMAT_MJPG);
		}

		ret = hd_videoenc_release_out_buf(output->enc_path, &bitstream);
		if (ret != HD_OK) {
			output->release_errors++;
			printf("UVC encoder release failed=%d\n", ret);
			output->failed = 1;
			break;
		}
		output->bitstream_held = 0;
		output->released++;
		if (send_ret != HD_OK) {
			printf("UVC delivery failed=%d; Stop/Restart required\n", send_ret);
			output->failed = 1;
			break;
		}
	}

	ret = hd_common_mem_munmap((void *)encoder_va,
		encoder_buffer.buf_info.buf_size);
	if (ret != HD_OK) {
		printf("unmap UVC MJPEG buffer failed=%d\n", ret);
		output->failed = 1;
	} else {
		output->encoder_va = 0;
	}
	return NULL;
}

static void uvc_output_reset(UVC_OUTPUT *output)
{
	memset(output, 0, sizeof(*output));
}

HD_RESULT uvc_output_request_mjpeg_snapshot(UVC_OUTPUT *output,
	const char *file_path)
{
	size_t path_length;
	HD_RESULT ret = HD_OK;

	if (output == NULL || file_path == NULL || output != g_uvc_output ||
	    !output->thread_started || !output->encoder_started) {
		return HD_ERR_NG;
	}
	path_length = strlen(file_path);
	if (path_length == 0 || path_length >= UVC_DIAGNOSTIC_PATH_SIZE) {
		printf("UVC MJPEG diagnostic path is invalid\n");
		return HD_ERR_NG;
	}

	pthread_mutex_lock(&g_uvc_diagnostic_lock);
	if (g_uvc_mjpeg_snapshot_pending) {
		ret = HD_ERR_NG;
	} else {
		memcpy(g_uvc_mjpeg_snapshot_path, file_path, path_length + 1U);
		g_uvc_mjpeg_snapshot_pending = 1;
	}
	pthread_mutex_unlock(&g_uvc_diagnostic_lock);

	if (ret != HD_OK) {
		printf("UVC MJPEG diagnostic request is already pending\n");
	} else {
		printf("UVC MJPEG diagnostic requested: %s\n", file_path);
	}
	return ret;
}

HD_RESULT uvc_output_open(UVC_OUTPUT *output, UINT32 width, UINT32 height,
	UINT32 fps, UINT32 isp_id)
{
	UVAC_INFO uvac_info = {0};
	UVAC_VEND_DEV_DESC device_desc = {0};
	UVAC_VID_RESO_ARY resolution_array = {0};
	UVAC_AUD_SAMPLERATE_ARY audio_rate_array = {0};
	void *buffer_va = NULL;
	uintptr_t buffer_pa = 0;
	UINT32 open_ret;
	HD_RESULT ret;

	if (output == NULL || width == 0 || height == 0 ||
	    fps == 0 || fps > UVC_OUTPUT_MAX_FPS) {
		printf("Invalid UVC configuration: %ux%u %u fps\n",
			width, height, fps);
		return HD_ERR_NG;
	}

	uvc_cancel_mjpeg_snapshot();
	uvc_output_reset(output);
	output->width = width;
	output->height = height;
	output->fps = fps;
	output->frame_size = UVC_MJPEG_MAX_FRAME_SIZE;

	ret = hd_videoenc_open(HD_VIDEOENC_0_IN_0, HD_VIDEOENC_0_OUT_0,
		&output->enc_path);
	if (ret != HD_OK) {
		printf("open UVC MJPEG encoder failed=%d\n", ret);
		goto fail;
	}
	ret = uvc_encoder_configure(output->enc_path, width, height, fps, isp_id);
	if (ret != HD_OK) {
		goto fail;
	}

	g_uvc_mjpg_resolution[0].width = width;
	g_uvc_mjpg_resolution[0].height = height;
	g_uvc_mjpg_resolution[0].fps[0] = fps;
	g_uvc_h264_resolution[0].width = width;
	g_uvc_h264_resolution[0].height = height;
	g_uvc_h264_resolution[0].fps[0] = fps;

	resolution_array.aryCnt = sizeof(g_uvc_h264_resolution) /
		sizeof(g_uvc_h264_resolution[0]);
	resolution_array.pVidResAry = g_uvc_h264_resolution;
	UVAC_SetConfig(UVAC_CONFIG_UVC_H264_FRM_INFO,
		(ULONG)&resolution_array);
	UVAC_SetConfig(UVAC_CONFIG_UVC_HS_H264_FRM_INFO,
		(ULONG)&resolution_array);

	resolution_array.aryCnt = sizeof(g_uvc_mjpg_resolution) /
		sizeof(g_uvc_mjpg_resolution[0]);
	resolution_array.pVidResAry = g_uvc_mjpg_resolution;
	UVAC_SetConfig(UVAC_CONFIG_UVC_MJPG_FRM_INFO,
		(ULONG)&resolution_array);
	UVAC_SetConfig(UVAC_CONFIG_UVC_HS_MJPG_FRM_INFO,
		(ULONG)&resolution_array);
	UVAC_SetConfig(UVAC_CONFIG_VIDEO_FORMAT_TYPE,
		(ULONG)UVAC_VIDEO_FORMAT_H264_MJPEG);
	UVAC_SetConfig(UVAC_CONFIG_MAX_FRAME_SIZE,
		(ULONG)UVC_MJPEG_MAX_FRAME_SIZE);

	audio_rate_array.aryCnt = sizeof(g_uvc_audio_sample_rates) /
		sizeof(g_uvc_audio_sample_rates[0]);
	audio_rate_array.pAudSampleRateAry = g_uvc_audio_sample_rates;
	UVAC_SetConfig(UVAC_CONFIG_AUD_SAMPLERATE,
		(ULONG)&audio_rate_array);
	UVAC_SetConfig(UVAC_CONFIG_AUD_CHANNEL_NUM,
		(ULONG)HD_AUDIO_SOUND_MODE_MONO);
	UVAC_SetConfig(UVAC_CONFIG_AUD_START_CB,
		(ULONG)uvc_start_audio_cb);

	device_desc.pManuStringDesc = (UVAC_STRING_DESC *)g_uvc_manufacturer;
	device_desc.pProdStringDesc = (UVAC_STRING_DESC *)g_uvc_product;
	device_desc.pSerialStringDesc = (UVAC_STRING_DESC *)g_uvc_serial;
	device_desc.VID = UVC_USB_VID;
	device_desc.PID = UVC_USB_PID;
	UVAC_SetConfig(UVAC_CONFIG_VEND_DEV_DESC, (ULONG)&device_desc);

	output->uvac_size = UVAC_GetNeedMemSize();
	if (output->uvac_size == 0) {
		printf("UVAC_GetNeedMemSize returned zero\n");
		ret = HD_ERR_NG;
		goto fail;
	}
	ret = hd_common_mem_alloc("tiffany_uvc", &buffer_pa, &buffer_va,
		output->uvac_size, DDR_ID0);
	if (ret != HD_OK) {
		printf("allocate UVC memory failed=%d size=%u\n",
			ret, output->uvac_size);
		goto fail;
	}
	output->uvac_pa = buffer_pa;
	output->uvac_va = (uintptr_t)buffer_va;

	buffer_pa = 0;
	buffer_va = NULL;
	ret = hd_common_mem_alloc("tiffany_uvc_frame", &buffer_pa, &buffer_va,
		output->frame_size, DDR_ID0);
	if (ret != HD_OK) {
		printf("allocate UVC MJPEG frame memory failed=%d size=%u\n",
			ret, output->frame_size);
		goto fail;
	}
	output->frame_pa = buffer_pa;
	output->frame_va = (uintptr_t)buffer_va;

	uvac_info.UvacMemAdr = output->uvac_pa;
	uvac_info.UvacMemSize = output->uvac_size;
	uvac_info.fpSetVolCB = (UVAC_SETVOLCB)uvc_set_volume_cb;
	uvac_info.fpStartVideoCB = (UVAC_STARTVIDEOCB)uvc_start_video_cb;
	uvac_info.fpStopVideoCB = (UVAC_STOPVIDEOCB)uvc_stop_video_cb;
	uvac_info.strmInfo.strmWidth = width;
	uvac_info.strmInfo.strmHeight = height;
	uvac_info.strmInfo.strmFps = fps;
	uvac_info.strmInfo.strmCodec = UVAC_VIDEO_FORMAT_MJPG;
	uvac_info.channel = UVAC_CHANNEL_1V1A;

	g_uvc_output = output;
	open_ret = UVAC_Open(&uvac_info);
	if (open_ret != E_OK) {
		printf("UVAC_Open failed=%u\n", open_ret);
		g_uvc_output = NULL;
		ret = HD_ERR_NG;
		goto fail;
	}
	output->uvac_opened = 1;
	printf("UVC device ready: MJPEG %ux%u %u fps quality=%u\n",
		width, height, fps, UVC_MJPEG_QUALITY);
	return HD_OK;

fail:
	if (output->frame_pa != 0 && output->frame_va != 0) {
		hd_common_mem_free(output->frame_pa, (void *)output->frame_va);
	}
	if (output->uvac_pa != 0 && output->uvac_va != 0) {
		hd_common_mem_free(output->uvac_pa, (void *)output->uvac_va);
	}
	if (output->enc_path != 0) {
		hd_videoenc_close(output->enc_path);
	}
	uvc_output_reset(output);
	return ret;
}

HD_RESULT uvc_output_start(UVC_OUTPUT *output)
{
	HD_RESULT ret;
	int thread_ret;

	if (output == NULL || !output->uvac_opened || output->enc_path == 0) {
		return HD_ERR_NG;
	}
	if (output->thread_started) {
		return HD_OK;
	}

	ret = hd_videoenc_start(output->enc_path);
	if (ret != HD_OK) {
		printf("start UVC MJPEG encoder failed=%d\n", ret);
		return ret;
	}
	output->encoder_started = 1;
	output->thread_exit = 0;
	output->failed = 0;
	thread_ret = pthread_create(&output->thread_id, NULL,
		uvc_output_thread, output);
	if (thread_ret != 0) {
		printf("create UVC output thread failed=%d\n", thread_ret);
		hd_videoenc_stop(output->enc_path);
		output->encoder_started = 0;
		return HD_ERR_NG;
	}
	output->thread_started = 1;
	return HD_OK;
}

HD_RESULT uvc_output_stop(UVC_OUTPUT *output)
{
	HD_RESULT ret = HD_OK;

	if (output == NULL) {
		return HD_ERR_NG;
	}

	output->host_streaming = 0;
	output->host_format = 0;
	uvc_cancel_mjpeg_snapshot();
	output->thread_exit = 1;
	if (output->thread_started) {
		pthread_join(output->thread_id, NULL);
		output->thread_started = 0;
		output->thread_id = (pthread_t)0;
	}
	/* USB owns frame_va until completion or a synchronous UVAC close. */
	if (output->uvac_frame_pending && output->uvac_opened) {
		UVAC_Close();
		output->uvac_opened = 0;
		output->uvac_frame_pending = 0;
	}
	if (output->bitstream_held) {
		HD_VIDEOENC_BS descriptor = output->held_bitstream;
		ret = hd_videoenc_release_out_buf(output->enc_path, &descriptor);
		if (ret != HD_OK) { output->release_errors++; return ret; }
		output->bitstream_held = 0;
		output->released++;
	}
    if (FRAME_DIAGNOSTICS_ENABLED)
    printf("UVC ownership: pull=%llu release=%llu held=%d release_error=%llu wait_error=%llu\n",
		output->pulled, output->released, output->bitstream_held,
		output->release_errors, output->wait_errors);
	if (output->encoder_va) {
		ret = hd_common_mem_munmap((void *)output->encoder_va, output->encoder_map_size);
		if (ret != HD_OK) return ret;
		output->encoder_va = 0;
	}
	if (output->encoder_started) {
		ret = hd_videoenc_stop(output->enc_path);
		if (ret != HD_OK) {
			printf("stop UVC MJPEG encoder failed=%d\n", ret);
			return ret;
		}
		output->encoder_started = 0;
	}
	return ret;
}

HD_RESULT uvc_output_close(UVC_OUTPUT *output)
{
	HD_RESULT ret;
	HD_RESULT close_ret;

	if (output == NULL) {
		return HD_ERR_NG;
	}

	ret = uvc_output_stop(output);
	if (ret != HD_OK) return ret;
	if (output->uvac_opened) {
		UVAC_Close();
		output->uvac_opened = 0;
	}
	if (g_uvc_output == output) {
		g_uvc_output = NULL;
	}
	if (output->enc_path != 0) {
		close_ret = hd_videoenc_close(output->enc_path);
		if (close_ret != HD_OK) {
			printf("close UVC MJPEG encoder failed=%d\n", close_ret);
			return close_ret;
		}
		output->enc_path = 0;
	}
	if (output->frame_pa != 0 && output->frame_va != 0) {
		close_ret = hd_common_mem_free(output->frame_pa,
			(void *)output->frame_va);
		if (close_ret != HD_OK) return close_ret;
		output->frame_pa = output->frame_va = 0;
	}
	if (output->uvac_pa != 0 && output->uvac_va != 0) {
		close_ret = hd_common_mem_free(output->uvac_pa,
			(void *)output->uvac_va);
		if (close_ret != HD_OK) return close_ret;
		output->uvac_pa = output->uvac_va = 0;
	}
	uvc_output_reset(output);
	return ret;
}
