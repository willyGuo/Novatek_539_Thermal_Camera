#include "rtsp_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#define RTSP_SERVICE_SHM_SIZE 0x1000
#define RTSP_SERVICE_SHM_KEY  8001
#define RTSP_SERVICE_PD_MAX   32
#define RTSP_SERVICE_PATH     "/bin/nvtrtspd_ipc"
#define RTSP_ENCODER_GOP      60
#define RTSP_ENCODER_BUF_MS   3000

/* Keep this layout identical to pd_shm.h used by nvtrtspd_ipc. */
typedef struct {
	int category;
	float score;
	int x1;
	int y1;
	int x2;
	int y2;
} RTSP_SERVICE_PD_RESULT;

typedef struct {
	unsigned int init_tag;
	HD_DIM pd_path;
	HD_DIM enc_path[3];
	unsigned int pd_num;
	RTSP_SERVICE_PD_RESULT pd_results[RTSP_SERVICE_PD_MAX];
	int exit;
} RTSP_SERVICE_SHM_INFO;

typedef char RTSP_SERVICE_SHM_SIZE_CHECK[
	(sizeof(RTSP_SERVICE_SHM_INFO) <= RTSP_SERVICE_SHM_SIZE) ? 1 : -1];

static void rtsp_output_reset(RTSP_OUTPUT *output)
{
	memset(output, 0, sizeof(*output));
	output->service_shmid = -1;
}

static void rtsp_service_release_shm(RTSP_OUTPUT *output)
{
	if (output->service_shm != NULL) {
		shmdt(output->service_shm);
		output->service_shm = NULL;
	}

	if (output->service_shm_created) {
		shmctl(output->service_shmid, IPC_RMID, NULL);
		output->service_shm_created = 0;
	}
	output->service_shmid = -1;
}

static int rtsp_service_prepare_shm(RTSP_OUTPUT *output)
{
	output->service_shmid = shmget(RTSP_SERVICE_SHM_KEY,
		RTSP_SERVICE_SHM_SIZE, IPC_CREAT | 0666);
	if (output->service_shmid < 0) {
		perror("RTSP shmget");
		return -1;
	}
	output->service_shm_created = 1;

	output->service_shm = shmat(output->service_shmid, NULL, 0);
	if (output->service_shm == (void *)-1) {
		output->service_shm = NULL;
		perror("RTSP shmat");
		rtsp_service_release_shm(output);
		return -1;
	}

	memset(output->service_shm, 0, RTSP_SERVICE_SHM_SIZE);
	return 0;
}

static HD_RESULT rtsp_output_set_path_config(HD_PATH_ID enc_path,
	UINT32 width, UINT32 height, UINT32 bitrate, UINT32 isp_id)
{
	HD_VIDEOENC_PATH_CONFIG path_config = {0};
	HD_VIDEOENC_FUNC_CONFIG func_config = {0};
	HD_RESULT ret;

	path_config.max_mem.codec_type = HD_CODEC_TYPE_H265;
	path_config.max_mem.max_dim.w = width;
	path_config.max_mem.max_dim.h = height;
	path_config.max_mem.bitrate = bitrate;
	path_config.max_mem.enc_buf_ms = RTSP_ENCODER_BUF_MS;
	path_config.max_mem.svc_layer = HD_SVC_4X;
	path_config.max_mem.ltr = TRUE;
	path_config.max_mem.rotate = FALSE;
	path_config.max_mem.source_output = FALSE;
	path_config.isp_id = isp_id;

	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_PATH_CONFIG,
		&path_config);
	if (ret != HD_OK) {
		printf("set RTSP encoder path config failed=%d\n", ret);
		return ret;
	}

	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_FUNC_CONFIG,
		&func_config);
	if (ret != HD_OK) {
		printf("set RTSP encoder function config failed=%d\n", ret);
	}
	return ret;
}

static HD_RESULT rtsp_output_set_encode_param(HD_PATH_ID enc_path,
	UINT32 width, UINT32 height, UINT32 fps, UINT32 bitrate)
{
	HD_VIDEOENC_IN input = {0};
	HD_VIDEOENC_OUT output = {0};
	HD_H26XENC_RATE_CONTROL rate_control = {0};
	HD_H26XENC_VUI vui = {0};
	HD_RESULT ret;

	input.dir = HD_VIDEO_DIR_NONE;
	input.pxl_fmt = HD_VIDEO_PXLFMT_YUV420;
	input.dim.w = width;
	input.dim.h = height;
	input.frc = HD_VIDEO_FRC_RATIO(1, 1);
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_IN, &input);
	if (ret != HD_OK) {
		printf("set RTSP encoder input failed=%d\n", ret);
		return ret;
	}

	output.codec_type = HD_CODEC_TYPE_H265;
	output.h26x.profile = HD_H265E_MAIN_PROFILE;
	output.h26x.level_idc = HD_H265E_LEVEL_5;
	output.h26x.gop_num = RTSP_ENCODER_GOP;
	output.h26x.ltr_interval = 0;
	output.h26x.ltr_pre_ref = 0;
	output.h26x.gray_en = 0;
	output.h26x.source_output = 0;
	output.h26x.svc_layer = HD_SVC_DISABLE;
	output.h26x.entropy_mode = HD_H265E_CABAC_CODING;
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_OUT_ENC_PARAM,
		&output);
	if (ret != HD_OK) {
		printf("set RTSP H.265 parameter failed=%d\n", ret);
		return ret;
	}

	rate_control.rc_mode = HD_RC_MODE_CBR;
	rate_control.cbr.bitrate = bitrate;
	rate_control.cbr.frame_rate_base = fps;
	rate_control.cbr.frame_rate_incr = 1;
	rate_control.cbr.init_i_qp = 26;
	rate_control.cbr.min_i_qp = 20;
	rate_control.cbr.max_i_qp = 45;
	rate_control.cbr.init_p_qp = 26;
	rate_control.cbr.min_p_qp = 20;
	rate_control.cbr.max_p_qp = 45;
	rate_control.cbr.static_time = 4;
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_OUT_RATE_CONTROL,
		&rate_control);
	if (ret != HD_OK) {
		printf("set RTSP encoder rate control failed=%d\n", ret);
		return ret;
	}

	vui.vui_en = TRUE;
	vui.color_range = 1;
	ret = hd_videoenc_set(enc_path, HD_VIDEOENC_PARAM_OUT_VUI, &vui);
	if (ret != HD_OK) {
		printf("set RTSP encoder VUI failed=%d\n", ret);
	}
	return ret;
}

HD_RESULT rtsp_output_open(RTSP_OUTPUT *output, UINT32 width, UINT32 height,
	UINT32 fps, UINT32 bitrate, UINT32 isp_id)
{
	HD_RESULT ret;

	if (output == NULL || width == 0 || height == 0 ||
	    fps == 0 || bitrate == 0) {
		return HD_ERR_NG;
	}

	rtsp_output_reset(output);
	ret = hd_videoenc_open(HD_VIDEOENC_0_IN_0, HD_VIDEOENC_0_OUT_0,
		&output->enc_path);
	if (ret != HD_OK) {
		printf("open RTSP encoder failed=%d\n", ret);
		return ret;
	}

	ret = rtsp_output_set_path_config(output->enc_path, width, height,
		bitrate, isp_id);
	if (ret != HD_OK) {
		goto fail;
	}

	ret = rtsp_output_set_encode_param(output->enc_path, width, height,
		fps, bitrate);
	if (ret != HD_OK) {
		goto fail;
	}
	printf("RTSP encoder: H.265 %ux%u %u fps %u bps\n",
		width, height, fps, bitrate);
	return HD_OK;

fail:
	if (output->enc_path != 0) {
		hd_videoenc_close(output->enc_path);
	}
	rtsp_output_reset(output);
	return ret;
}

HD_RESULT rtsp_output_start(RTSP_OUTPUT *output)
{
	HD_RESULT ret;
	int service_ret;

	if (output == NULL || output->enc_path == 0) {
		return HD_ERR_NG;
	}

	ret = hd_videoenc_start(output->enc_path);
	if (ret != HD_OK) {
		printf("start RTSP encoder failed=%d\n", ret);
		return ret;
	}
	output->encoder_started = 1;

	if (rtsp_service_prepare_shm(output) != 0) {
		hd_videoenc_stop(output->enc_path);
		output->encoder_started = 0;
		return HD_ERR_NG;
	}

	if (access(RTSP_SERVICE_PATH, X_OK) != 0) {
		perror("RTSP service is not executable");
		rtsp_service_release_shm(output);
		hd_videoenc_stop(output->enc_path);
		output->encoder_started = 0;
		return HD_ERR_NG;
	}

	service_ret = system(RTSP_SERVICE_PATH " &");
	if (service_ret != 0) {
		printf("start nvtrtspd_ipc failed=%d\n", service_ret);
		rtsp_service_release_shm(output);
		hd_videoenc_stop(output->enc_path);
		output->encoder_started = 0;
		return HD_ERR_NG;
	}

	output->service_started = 1;
	printf("RTSP service started\n");
	return HD_OK;
}

HD_RESULT rtsp_output_stop(RTSP_OUTPUT *output)
{
	HD_RESULT ret = HD_OK;

	if (output == NULL) {
		return HD_ERR_NG;
	}

	if (output->service_started) {
		if (output->service_shm != NULL) {
			RTSP_SERVICE_SHM_INFO *service_info =
				(RTSP_SERVICE_SHM_INFO *)output->service_shm;
			service_info->exit = 1;
		}
		usleep(300000);
		output->service_started = 0;
	}
	rtsp_service_release_shm(output);

	if (output->encoder_started) {
		ret = hd_videoenc_stop(output->enc_path);
		if (ret != HD_OK) {
			printf("stop RTSP encoder failed=%d\n", ret);
		}
		output->encoder_started = 0;
	}

	return ret;
}

HD_RESULT rtsp_output_close(RTSP_OUTPUT *output)
{
	HD_RESULT ret;

	if (output == NULL) {
		return HD_ERR_NG;
	}

	ret = rtsp_output_stop(output);
	if (output->enc_path != 0) {
		HD_RESULT close_ret = hd_videoenc_close(output->enc_path);
		if (close_ret != HD_OK) {
			printf("close RTSP encoder failed=%d\n", close_ret);
			if (ret == HD_OK) {
				ret = close_ret;
			}
		}
	}
	rtsp_output_reset(output);
	return ret;
}
