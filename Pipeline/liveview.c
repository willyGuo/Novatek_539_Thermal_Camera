/**
	@brief Sample code of video liveview with thermal sensor.\n

	@file liveview.c

	@author Willy Guo

	@ingroup mhdal

	@note This file is modified from video_liveview_with_mode.c.

	Copyright Novatek Microelectronics Corp. 2024.  All rights reserved.
*/

#include "liveview.h"
#include "../Enhancement/CLAHE/clahe_frame.h"
#include "../Enhancement/DDE/dde_frame.h"
#include "../ISP/Pseudo/pseudo.h"
#include "../ISP/VPE/vpe_mirror.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#if defined(__LINUX)
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#endif
#include "hdal.h"
#include "vendor_videocapture.h"
#include "vendor_videoprocess.h"
#include "../Config/app_config.h"
#include "../Config/hardware_config.h"
#include "../Device/FOS/fos.h"
#include "../Device/SensorTemp/sensor_temp.h"
#include "vendor_isp.h"
#include "../Correction/BPR/bpr.h"
#include "../Correction/FFC/ffc.h"
#include "../Correction/NUC/nuc.h"
#include "../Correction/TDC/tdc.h"
#include "../Enhancement/AGC/agc_fieldscale.h"
#include "../Enhancement/AGC/agc_histogram_p1_p99.h"
#include "../Enhancement/AGC/agc_min_max_tone_curve.h"
#include "../Enhancement/AGC/agc_mode.h"
#include "../Enhancement/Gamma/gamma.h"
#include "../ISP/videoproc/videoproc_isp.h"
#include "../Diagnostics/ISP/isp_raw_to_y_step_diagnostic.h"
#include "../Diagnostics/timed_snapshot.h"
#include "../Diagnostics/minute_raw_recording.h"
#include "../Diagnostics/raw_recording_log.h"
#include "../Driver/GPIO/gpio_control.h"
#include "Output/video_output.h"

#define MENU_OPTION_REFRESH (-2)

// platform dependent
#if defined(__LINUX)
#include <pthread.h>			//for pthread API
#else
#include <FreeRTOS_POSIX.h>
#include <FreeRTOS_POSIX/pthread.h> //for pthread API
#include <kwrap/util.h>		//for sleep API
#define sleep(x)    			vos_util_delay_ms(1000*(x))
#define usleep(x)   			vos_util_delay_us(x)
#endif


///////////////////////////////////////////////////////////////////////////////

//header
#define DBGINFO_BUFSIZE()	(0x200)

//RAW
#define VDO_RAW_BUFSIZE(w, h, pxlfmt)   (ALIGN_CEIL_4((w) * HD_VIDEO_PXLFMT_BPP(pxlfmt) / 8) * (h))
//CA for AWB
#define VDO_CA_BUF_SIZE(win_num_w, win_num_h) ALIGN_CEIL_4((win_num_w * win_num_h << 3) << 1)
//LA for AE
#define VDO_LA_BUF_SIZE(win_num_w, win_num_h) ALIGN_CEIL_4((win_num_w * win_num_h << 1) << 1)

//YUV
#define VDO_YUV_BUFSIZE(w, h, pxlfmt)	(ALIGN_CEIL_4((w) * HD_VIDEO_PXLFMT_BPP(pxlfmt) / 8) * (h))
///////////////////////////////////////////////////////////////////////////////

#define SEN_OUT_FMT     HD_VIDEO_PXLFMT_RAW16
#define CAP_OUT_FMT     (HD_VIDEO_PXLFMT_RAW16 | HD_VIDEO_PIX_PACK_LSB)
#define CA_WIN_NUM_W    32
#define CA_WIN_NUM_H    32
#define LA_WIN_NUM_W    32
#define LA_WIN_NUM_H    32

#define TY_640          0
#define OS05A10         1
#define RESOLUTION_SET  TY_640

#define DEFAULT_STARTUP_SKIP_FRAMES   30
#define DEFAULT_RUNTIME_SKIP_FRAMES   0
#define DEFAULT_SHUTTER_CLOSE_WAIT_MS 200
#define SHUTTER_CALIBRATION_PAUSED    1
#define YUV_STATS_WAIT_TIMEOUT_MS     1000U
#define ISP_STEP_TEST_WAIT_TIMEOUT_SEC 30U

#define THERMAL_SNAPSHOT_DIR          "/mnt/sd/thermal_snapshot"
#define RAW_SNAPSHOT_DIR              THERMAL_SNAPSHOT_DIR "/raw"
#define DISPLAY_DIAG_DIR              THERMAL_SNAPSHOT_DIR "/display_diag"
#define SCHEDULED_TIMED_SNAPSHOT 1
#define SCHEDULED_MINUTE_RAW     2

#define SNAPSHOT_PREFIX               "snapshot_"
#define DISPLAY_DIAG_PREFIX           "display_diag_"
#define SNAPSHOT_FRAME_BYTES          \
	((size_t)RAW_ACTIVE_W * RAW_ACTIVE_H * sizeof(UINT16))

#define FFC_AVERAGE_FRAME_COUNT 4U
#define FFC_AVERAGE_SHIFT       2U
#define FFC_TEST_SAMPLE_COUNT   10U

#if (AGC_MIN_MAX_TONE_CURVE_LEFT_LUT_COUNT != IQ_TONE_LEFT_NUM)
#error "Min/Max Tone Curve AGC left LUT size does not match the ISP SDK"
#endif

#if (AGC_MIN_MAX_TONE_CURVE_RIGHT_LUT_COUNT != IQ_TONE_RIGHT_NUM)
#error "Min/Max Tone Curve AGC right LUT size does not match the ISP SDK"
#endif

#if (FFC_AVERAGE_FRAME_COUNT != 4U)
#error "FFC averaging currently supports exactly 4 frames"
#endif

#if (FFC_AVERAGE_SHIFT != 2U)
#error "Four-frame FFC averaging requires shift 2"
#endif

/* BSP-verified TY640 active resolution used to select the sensor mode. */
#define SENSOR_MODE_W           640
#define SENSOR_MODE_H           512

/*
 * The TY640 VCAP driver returns the active image directly.  Board metadata
 * reports dim=640x512 and loff=1280 bytes, so there are no dummy columns or
 * rows in the frame handed to this pipeline.
 */
#define RAW_SIZE_W              SENSOR_MODE_W
#define RAW_SIZE_H              SENSOR_MODE_H

#if (RESOLUTION_SET == TY_640)
#define VDO_SIZE_W      RAW_SIZE_W
#define VDO_SIZE_H      RAW_SIZE_H
#endif

/* The complete VCAP frame is also the 640x512 active image. */
#define RAW_ACTIVE_X            0
#define RAW_ACTIVE_Y            0
#define RAW_ACTIVE_W            RAW_SIZE_W
#define RAW_ACTIVE_H            RAW_SIZE_H

#define TDC_blk_size  (RAW_SIZE_W * RAW_SIZE_H * sizeof(float))

#define VIDEO_OUTPUT_WIDTH       RAW_ACTIVE_W
#define VIDEO_OUTPUT_HEIGHT      RAW_ACTIVE_H
#define RTSP_OUTPUT_BITRATE_BPS  (4U * 1024U * 1024U)

#define VIDEOCAP_ALG_FUNC 0//HD_VIDEOCAP_FUNC_AE | HD_VIDEOCAP_FUNC_AWB

static UINT32 g_fps = 30;
static VIDEO_OUTPUT_MODE g_output_mode = VIDEO_OUTPUT_MODE_UVC;
static int g_liveview_mirror = 1;

static UINT32 g_startup_skip_frames = DEFAULT_STARTUP_SKIP_FRAMES;
/* VCAP frames dropped after each frame successfully submitted to VideoProc. */
static UINT32 g_runtime_skip_frames = DEFAULT_RUNTIME_SKIP_FRAMES;
static UINT32 g_shutter_close_wait_ms = DEFAULT_SHUTTER_CLOSE_WAIT_MS;

static volatile int g_raw_copy_pending = 0;
static volatile int g_raw_copy_done = 0;
static volatile int g_raw_copy_result = 0;
static UINTPTR g_raw_copy_dst_va = 0;
static UINT32 g_raw_copy_size = 0;
static volatile int g_yuv_stats_pending = 0;
static UINT32 g_yuv_diag_id = 0;
static UINT32 g_display_diag_count = 0;
static VIDEO_OUTPUT *g_diagnostic_video_output = NULL;
static int g_proc_input_format_logged = 0;
static UINT64 g_shutter_next_calibration_ms = 0;
static int g_manual_calibration_active = 0;
/* Protected by g_manual_calibration_lock; consumed only after successful NUC. */
static int g_nuc_snapshot_pending = 0;
static int g_shutter_reschedule_after_manual_calibration = 0;
static int display_diag_get_next_id(UINT32 *p_diag_id);
///////////////////////////////////////////////////////////////////////////////

static int liveview_prepare_output_directories(void)
{
#if defined(__LINUX)
	const char *directories[] = {
		THERMAL_SNAPSHOT_DIR,
		RAW_SNAPSHOT_DIR,
		DISPLAY_DIAG_DIR,
	};
	size_t i;

	for (i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		struct stat path_stat;

		if (mkdir(directories[i], 0775) == 0) {
			continue;
		}
		if (errno == EEXIST && stat(directories[i], &path_stat) == 0 &&
		    S_ISDIR(path_stat.st_mode)) {
			continue;
		}
		printf("create output directory %s fail: %s\n",
		       directories[i], strerror(errno));
		return -1;
	}
#endif

	return 0;
}

static UINT32 liveview_get_videoproc_alg_func(void)
{
	UINT32 func = 0;

	if (videoproc_isp_is_af_enabled()) {
		func |= HD_VIDEOPROC_FUNC_AF;
	}
	if (videoproc_isp_is_wdr_enabled()) {
		func |= HD_VIDEOPROC_FUNC_WDR;
	}
	if (videoproc_isp_is_defog_enabled()) {
		func |= HD_VIDEOPROC_FUNC_DEFOG;
	}
	if (videoproc_isp_is_3dnr_enabled()) {
		func |= HD_VIDEOPROC_FUNC_3DNR |
			HD_VIDEOPROC_FUNC_3DNR_STA;
	}

	return func;
}

static int liveview_request_raw_copy(UINTPTR dst_va, UINT32 size)
{
	UINT32 wait_count = 0;

	if (dst_va == 0 || size == 0) {
		return -1;
	}

	if (g_raw_copy_pending) {
		printf("raw copy request is busy\n");
		return -1;
	}

	g_raw_copy_dst_va = dst_va;
	g_raw_copy_size = size;
	g_raw_copy_result = -1;
	g_raw_copy_done = 0;
	g_raw_copy_pending = 1;

	while (!g_raw_copy_done && wait_count < 5000) {
		usleep(1000);
		wait_count++;
	}

	if (!g_raw_copy_done) {
		g_raw_copy_pending = 0;
		printf("raw copy request timeout\n");
		return -1;
	}

	return g_raw_copy_result;
}

static int liveview_copy_next_raw(UINTPTR dst_va, UINT32 size)
{
	return liveview_request_raw_copy(dst_va, size);
}

static int liveview_request_yuv_statistics(void)
{
	UINT32 wait_count = 0;
	UINT32 diag_id;
	char mjpeg_path[192];
	HD_RESULT encoded_ret = HD_ERR_NG;

	if (!liveview_is_running()) {
		printf("YUV statistics unavailable: Liveview is stopped\n");
		return -1;
	}

	if (g_yuv_stats_pending) {
		printf("YUV statistics request is already pending\n");
	} else {
		if (display_diag_get_next_id(&diag_id) != 0) {
			printf("allocate display diagnostic sequence fail\n");
			return -1;
		}
		g_yuv_diag_id = diag_id;
		g_yuv_stats_pending = 1;
		snprintf(mjpeg_path, sizeof(mjpeg_path),
			 DISPLAY_DIAG_DIR "/display_diag_%04u_uvc_mjpeg_%ux%u.jpg",
			 diag_id, VIDEO_OUTPUT_WIDTH, VIDEO_OUTPUT_HEIGHT);
		if (g_diagnostic_video_output != NULL) {
			encoded_ret = video_output_request_encoded_snapshot(
				g_diagnostic_video_output, mjpeg_path);
		}
		printf("Display diagnostic %04u requested for the next VideoProc "
		       "Y frame; MJPEG=%s\n", diag_id,
		       encoded_ret == HD_OK ? "requested" : "unavailable");
	}

	while (g_yuv_stats_pending && wait_count < YUV_STATS_WAIT_TIMEOUT_MS) {
		usleep(1000);
		wait_count++;
	}
	if (g_yuv_stats_pending) {
		printf("YUV statistics are still pending; waiting for a VideoProc frame\n");
	}

	return 0;
}

static UINT64 liveview_get_time_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (UINT64)ts.tv_sec * 1000 + (UINT64)ts.tv_nsec / 1000000;
}

static UINT64 liveview_get_time_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (UINT64)ts.tv_sec * 1000000U +
		(UINT64)ts.tv_nsec / 1000U;
}

static void liveview_sleep_ms(UINT32 duration_ms)
{
	while (duration_ms >= 500U) {
		usleep(500000U);
		duration_ms -= 500U;
	}
	if (duration_ms > 0) {
		usleep(duration_ms * 1000U);
	}
}

static void liveview_schedule_next_shutter_calibration(void)
{
	UINT32 interval_sec = ffc_get_shutter_interval_sec();

	if (interval_sec == 0) {
		g_shutter_next_calibration_ms = 0;
		return;
	}
	g_shutter_next_calibration_ms =
		liveview_get_time_ms() + (UINT64)interval_sec * 1000;
}


static HD_RESULT mem_init(void)
{
	HD_RESULT              ret;
	HD_COMMON_MEM_INIT_CONFIG mem_cfg = {0};
	UINT32 id;

	// config common pool (cap)
	id = 0;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	//normal
	mem_cfg.pool_info[id].blk_size = DBGINFO_BUFSIZE()
    													+VDO_CA_BUF_SIZE(CA_WIN_NUM_W, CA_WIN_NUM_H)
    													+VDO_LA_BUF_SIZE(LA_WIN_NUM_W, LA_WIN_NUM_H);

	mem_cfg.pool_info[id].blk_size += VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);
	mem_cfg.pool_info[id].blk_cnt = 2 + 11;
	// 11 => TDC out + NUC k + NUC b + NUC out + FFC k + FFC b + FFC out + FFC sum + FFC zero + shared CPU AGC out + ISP step test
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;

	// config common pool (main)
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = DBGINFO_BUFSIZE()+VDO_YUV_BUFSIZE(VIDEO_OUTPUT_WIDTH, VIDEO_OUTPUT_HEIGHT, HD_VIDEO_PXLFMT_YUV420);
	mem_cfg.pool_info[id].blk_cnt = 7; /* Includes VPE output/encoder-held buffers. */
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;

	// TDC common pool (main)
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = TDC_blk_size;
	mem_cfg.pool_info[id].blk_cnt = 4; // 需要分配給 DCC_K, LC_K, ADD_output, all_1:  4 個 Block
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;

	/* IVE pseudo color: 256-byte Y LUT and tightly packed Y output. */
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = 256;
	mem_cfg.pool_info[id].blk_cnt = 1;
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = RAW_ACTIVE_W * RAW_ACTIVE_H;
	mem_cfg.pool_info[id].blk_cnt = 1;
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;
	ret = hd_common_mem_init(&mem_cfg);
	return ret;
}

static HD_RESULT mem_exit(void)
{
	HD_RESULT ret = HD_OK;
	hd_common_mem_uninit();
	return ret;
}

///////////////////////////////////////////////////////////////////////////////

static HD_RESULT get_cap_caps(HD_PATH_ID video_cap_ctrl, HD_VIDEOCAP_SYSCAPS *p_video_cap_syscaps)
{
	HD_RESULT ret = HD_OK;
	hd_videocap_get(video_cap_ctrl, HD_VIDEOCAP_PARAM_SYSCAPS, p_video_cap_syscaps);
	return ret;
}

static HD_RESULT set_cap_cfg(HD_PATH_ID *p_video_cap_ctrl)
{
	HD_RESULT ret = HD_OK;
	HD_VIDEOCAP_DRV_CONFIG cap_cfg = {0};
	HD_PATH_ID video_cap_ctrl = 0;
	HD_VIDEOCAP_CTRL iq_ctl = {0};

    #if (RESOLUTION_SET == TY_640)
	snprintf(cap_cfg.sen_cfg.sen_dev.driver_name, HD_VIDEOCAP_SEN_NAME_LEN-1, "nvt_sen_ty640");
	printf("Using nvt_sen_ty640\n");
	#elif (RESOLUTION_SET == OS05A10)
	snprintf(cap_cfg.sen_cfg.sen_dev.driver_name, HD_VIDEOCAP_SEN_NAME_LEN-1, "nvt_sen_os05a10");
	printf("Using nvt_sen_os05a10\n");
	#endif

	cap_cfg.sen_cfg.sen_dev.if_type = HD_COMMON_VIDEO_IN_TDIO;
    cap_cfg.sen_cfg.sen_dev.pin_cfg.pinmux.sensor_pinmux =  0;  //use @0 in peri-dev.dtsi

	ret = hd_videocap_open(0, HD_VIDEOCAP_CTRL(g_thermal_pipeline_id), &video_cap_ctrl); //open this for device control

	if (ret != HD_OK) {
		return ret;
	}
	ret |= hd_videocap_set(video_cap_ctrl, HD_VIDEOCAP_PARAM_DRV_CONFIG, &cap_cfg);
	iq_ctl.func = VIDEOCAP_ALG_FUNC;

	ret |= hd_videocap_set(video_cap_ctrl, HD_VIDEOCAP_PARAM_CTRL, &iq_ctl);

	*p_video_cap_ctrl = video_cap_ctrl;
	return ret;
}

static HD_RESULT set_cap_param(HD_PATH_ID video_cap_path, HD_DIM *p_dim)
{
	HD_RESULT ret = HD_OK;
	{//select sensor mode, manually or automatically
		HD_VIDEOCAP_IN video_in_param = {0};

		video_in_param.sen_mode = HD_VIDEOCAP_SEN_MODE_AUTO; //auto select sensor mode by the parameter of HD_VIDEOCAP_PARAM_OUT
		video_in_param.frc = HD_VIDEO_FRC_RATIO(g_fps,1);
		video_in_param.dim.w = p_dim->w;
		video_in_param.dim.h = p_dim->h;
		video_in_param.pxlfmt = SEN_OUT_FMT;
		video_in_param.out_frame_num = HD_VIDEOCAP_SEN_FRAME_NUM_1;
		ret = hd_videocap_set(video_cap_path, HD_VIDEOCAP_PARAM_IN, &video_in_param);
		if (ret != HD_OK) {
			return ret;
		}
	}
	{
		HD_VIDEOCAP_CROP video_crop_param = {0};

		video_crop_param.mode = HD_CROP_OFF;
		ret = hd_videocap_set(video_cap_path, HD_VIDEOCAP_PARAM_IN_CROP, &video_crop_param);
	}
	{
		HD_VIDEOCAP_OUT video_out_param = {0};

		//without setting dim for no scaling, using original sensor out size
		video_out_param.pxlfmt = CAP_OUT_FMT;
		video_out_param.dir = HD_VIDEO_DIR_NONE;
		video_out_param.depth = 1;
		ret = hd_videocap_set(video_cap_path, HD_VIDEOCAP_PARAM_OUT, &video_out_param);
	}
	{
		HD_VIDEOCAP_FUNC_CONFIG video_path_param = {0};

		video_path_param.out_func = 0;
		video_path_param.ddr_id = DDR_ID0;
		ret = hd_videocap_set(video_cap_path, HD_VIDEOCAP_PARAM_FUNC_CONFIG, &video_path_param);
	}
	return ret;
}

///////////////////////////////////////////////////////////////////////////////

static HD_RESULT set_proc_cfg(HD_PATH_ID *p_video_proc_ctrl, HD_DIM* p_max_dim, HD_OUT_ID _out_id)
{
	HD_RESULT ret = HD_OK;
	HD_VIDEOPROC_DEV_CONFIG video_cfg_param = {0};
	HD_VIDEOPROC_CTRL video_ctrl_param = {0};
	HD_VIDEOPROC_LL_CONFIG video_ll_param = {0};
	HD_PATH_ID video_proc_ctrl = 0;
	UINT32 alg_func = liveview_get_videoproc_alg_func();

	ret = hd_videoproc_open(0, _out_id, &video_proc_ctrl); //open this for device control
	if (ret != HD_OK)
		return ret;

	if (p_max_dim != NULL ) {
		video_cfg_param.pipe = HD_VIDEOPROC_PIPE_RAWALL;
		video_cfg_param.isp_id = g_thermal_pipeline_id;
		video_cfg_param.ctrl_max.func = alg_func;
		video_cfg_param.in_max.func = 0;
		video_cfg_param.in_max.dim.w = p_max_dim->w;
		video_cfg_param.in_max.dim.h = p_max_dim->h;
		video_cfg_param.in_max.pxlfmt = CAP_OUT_FMT;
		video_cfg_param.in_max.frc = HD_VIDEO_FRC_RATIO(1,1);
		ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_DEV_CONFIG, &video_cfg_param);
		if (ret != HD_OK) {
			return HD_ERR_NG;
		}
	}
	{
		HD_VIDEOPROC_FUNC_CONFIG video_path_param = {0};

		video_path_param.in_func = 0;
		ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_FUNC_CONFIG, &video_path_param);
	}
	{
		VENDOR_VIDEOPROC_THERMAL_INFO thermal_info = {0};

		thermal_info.enable = TRUE;
		ret = vendor_videoproc_set(video_proc_ctrl, VENDOR_VIDEOPROC_PARAM_THERMAL_INFO, (VOID *)&thermal_info);
		if (ret != HD_OK) {
			printf("enable VideoProc thermal mode failed=%d\n", ret);
		} else {
			printf("VideoProc thermal mode enabled\n");
		}
	}


	video_ctrl_param.func = alg_func;
	if (videoproc_isp_is_3dnr_enabled()) {
		video_ctrl_param.ref_path_3dnr = HD_VIDEOPROC_0_OUT_4;
	}
	ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_CTRL, &video_ctrl_param);

	video_ll_param.delay_trig_lowlatency = 0;
	ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_LL_CONFIG, &video_ll_param);
	printf("VideoProc / ISP runtime: AF=%s, WDR=%s, DEFOG=%s, 3DNR=%s, func=0x%x\n",
	       videoproc_isp_is_af_enabled() ? "on" : "off",
	       videoproc_isp_is_wdr_enabled() ? "on" : "off",
	       videoproc_isp_is_defog_enabled() ? "on" : "off",
	       videoproc_isp_is_3dnr_enabled() ? "on" : "off",
	       alg_func);

	*p_video_proc_ctrl = video_proc_ctrl;

	return ret;
}

static HD_RESULT set_proc_param(HD_PATH_ID video_proc_path, HD_DIM* p_dim,
	HD_VIDEO_PXLFMT pxlfmt, UINT32 depth)
{
	HD_RESULT ret = HD_OK;

	if (p_dim != NULL) {
		HD_VIDEOPROC_OUT video_out_param = {0};
		video_out_param.func = 0;
		video_out_param.dim.w = p_dim->w;
		video_out_param.dim.h = p_dim->h;
		video_out_param.pxlfmt = pxlfmt;
		video_out_param.dir = HD_VIDEO_DIR_NONE;
		video_out_param.frc = HD_VIDEO_FRC_RATIO(1,1);
		video_out_param.depth = depth;
		ret = hd_videoproc_set(video_proc_path, HD_VIDEOPROC_PARAM_OUT, &video_out_param);
		if (ret != HD_OK) {
			return ret;
		}
	}

	{
		HD_VIDEOPROC_CROP  video_in_param = {0};

		/* VCAP already provides the 640x512 active image to VideoProc. */
		video_in_param.mode = HD_CROP_OFF;
		ret = hd_videoproc_set(video_proc_path, HD_VIDEOPROC_PARAM_IN_CROP, &video_in_param);
	}

	{
		HD_VIDEOPROC_FUNC_CONFIG video_path_param = {0};

		video_path_param.out_func = 0;
		ret = hd_videoproc_set(video_proc_path, HD_VIDEOPROC_PARAM_FUNC_CONFIG, &video_path_param);
	}
	return ret;
}

/* Shared VideoProc input: applies to all outputs, after RAW capture/correction. */
static HD_RESULT liveview_set_proc_input_direction(HD_PATH_ID path)
{
	HD_VIDEOPROC_IN input = {0};
	HD_RESULT ret;

	/* Preserve the configured/automatic input format, dimensions and frame rate. */
	ret = hd_videoproc_get(path, HD_VIDEOPROC_PARAM_IN, &input);
	if (ret != HD_OK) {
		printf("VideoProc input direction get failed: path=0x%x, error=%d\n",
		       (unsigned int)path, ret);
		return ret;
	}
	input.dir = HD_VIDEO_DIR_NONE; /* Mirror is applied by the separate YUV VPE. */
	ret = hd_videoproc_set(path, HD_VIDEOPROC_PARAM_IN, &input);
	if (ret != HD_OK) {
		printf("VideoProc input direction set failed: path=0x%x, dir=0x%x, error=%d\n",
		       (unsigned int)path, (unsigned int)input.dir, ret);
		return ret;
	}
	printf("RAWALL input direction=0x%x; display VPE mirror=%s\n",
	       (unsigned int)input.dir, g_liveview_mirror ? "on" : "off");
	return HD_OK;
}

static IQT_TONE_PARAM g_agc_baseline_tone;
static int g_agc_baseline_valid;

static HD_RESULT liveview_bypass_isp_tone_for_cpu_agc(void)
{
	IQT_TONE_PARAM tone = {0};
	IQT_TONE_PARAM verify = {0};
	HD_RESULT ret;
	UINT32 previous_enable;

	if (!agc_histogram_p1_p99_is_enabled() &&
	    !agc_fieldscale_is_enabled()) {
		return HD_OK;
	}

	tone.id = g_thermal_pipeline_id;
	ret = vendor_isp_get_iq(IQT_ITEM_TONE_PARAM, &tone);
	if (ret != HD_OK) {
		printf("%s: get ISP Tone Curve failed=%d\n",
		       agc_mode_name(agc_mode_get()), ret);
		return ret;
	}

	previous_enable = tone.tone.enable;
	if (previous_enable) {
		tone.tone.enable = 0;
		ret = vendor_isp_set_iq(IQT_ITEM_TONE_PARAM, &tone);
		if (ret != HD_OK) {
			printf("%s: disable ISP Tone Curve failed=%d\n",
			       agc_mode_name(agc_mode_get()), ret);
			return ret;
		}

		verify.id = g_thermal_pipeline_id;
		ret = vendor_isp_get_iq(IQT_ITEM_TONE_PARAM, &verify);
		if (ret != HD_OK || verify.tone.enable) {
			printf("%s: verify ISP Tone Curve bypass "
			       "failed, get=%d, enable=%u\n",
			       agc_mode_name(agc_mode_get()),
			       ret,
			       ret == HD_OK ?
				(unsigned int)verify.tone.enable : 1U);
			return ret == HD_OK ? HD_ERR_NG : ret;
		}
	}

	printf("%s: ISP Tone Curve bypassed "
	       "(previous enable=%u, mode=%u, auto_sel=%u)\n",
	       agc_mode_name(agc_mode_get()),
	       previous_enable,
	       (unsigned int)tone.tone.mode,
	       (unsigned int)tone.tone.auto_sel);
	return HD_OK;
}

///////////////////////////////////////////////////////////////////////////////

typedef struct _VIDEO_LIVEVIEW {

	// (1)
	HD_VIDEOCAP_SYSCAPS cap_syscaps;
	HD_PATH_ID cap_ctrl;
	HD_PATH_ID cap_path;

	HD_DIM  cap_dim;

	// RAW pipeline worker state
	pthread_t  cap_thread_id;
	UINT32	cap_enter;
	UINT32	cap_exit;
	volatile UINT32 cap_snap;
	pthread_t  proc_thread_id;
	UINT32	proc_enter;
	UINT32	proc_exit;

	// (2)
	HD_DIM  proc_max_dim;
	HD_PATH_ID proc_ctrl;
	HD_PATH_ID proc_path;
	HD_PATH_ID proc_path_3dnr;
	HD_VIDEO_FRAME vpe_held_input;
	int vpe_input_held;

	// (3) Selected UVC MJPEG or RTSP H.265 output
	VIDEO_OUTPUT video_output;


	HD_COMMON_MEM_VB_BLK thermal_nuc_out_blk;
	UINTPTR thermal_nuc_out_pa;
	UINTPTR thermal_nuc_out_va;

	HD_COMMON_MEM_VB_BLK thermal_tdc_out_blk;
	UINTPTR thermal_tdc_out_pa;
	UINTPTR thermal_tdc_out_va;

	HD_COMMON_MEM_VB_BLK thermal_k_frame_blk;
	UINTPTR thermal_k_frame_pa;
	UINTPTR thermal_k_frame_va;

	HD_COMMON_MEM_VB_BLK thermal_b_frame_blk;
	UINTPTR thermal_b_frame_pa;
	UINTPTR thermal_b_frame_va;

	HD_COMMON_MEM_VB_BLK thermal_ffc_out_blk;
	UINTPTR thermal_ffc_out_pa;
	UINTPTR thermal_ffc_out_va;
	HD_COMMON_MEM_VB_BLK thermal_ffc_sum_blk;
	UINTPTR thermal_ffc_sum_pa;
	UINTPTR thermal_ffc_sum_va;
	HD_COMMON_MEM_VB_BLK thermal_ffc_zero_blk;
	UINTPTR thermal_ffc_zero_pa;
	UINTPTR thermal_ffc_zero_va;

	HD_COMMON_MEM_VB_BLK thermal_ffc_k_frame_blk;
	UINTPTR thermal_ffc_k_frame_pa;
	UINTPTR thermal_ffc_k_frame_va;

	HD_COMMON_MEM_VB_BLK thermal_ffc_b_frame_blk;
	UINTPTR thermal_ffc_b_frame_pa;
	UINTPTR thermal_ffc_b_frame_va;

	HD_COMMON_MEM_VB_BLK thermal_agc_histogram_p1_p99_out_blk;
	UINTPTR thermal_agc_histogram_p1_p99_out_pa;
	UINTPTR thermal_agc_histogram_p1_p99_out_va;
	uint32_t *agc_histogram_p1_p99_workspace;
	AGC_HISTOGRAM_P1_P99_STATE agc_histogram_p1_p99_state;
	uint32_t *agc_fieldscale_horizontal_workspace;
	AGC_FIELDSCALE_STATE agc_fieldscale_state;
	UINT32 agc_histogram_p1_p99_frame_count;
	UINT32 agc_histogram_p1_p99_error_count;
	UINT32 agc_fieldscale_frame_count;
	UINT32 agc_fieldscale_error_count;
	UINT32 agc_min_max_tone_curve_frame_count;
	UINT32 agc_min_max_tone_curve_error_count;

	HD_COMMON_MEM_VB_BLK thermal_isp_step_test_blk;
	UINTPTR thermal_isp_step_test_pa;
	UINTPTR thermal_isp_step_test_va;

} VIDEO_LIVEVIEW;

typedef struct {
	VIDEO_LIVEVIEW stream[1];
	int initialized;
	int running;
	int result;
	int common_inited;
	int mem_inited;
	int modules_inited;
	int isp_inited;
	int thermal_frame_created;
	int module_open_attempted;
	int cap_thread_created;
	int proc_thread_created;
	int pseudo_started;
	int snapshot_writer_started;
	int proc_bound;
	int cap_started;
	int proc_started;
	int proc_3dnr_started;
} LIVEVIEW_CONTEXT;

typedef struct {
	pthread_t thread_id;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	unsigned char *input_buffer;
	unsigned char *corrected_buffer;
	unsigned char *agc_output_buffer;
	UINT32 snapshot_id;
	int final_is_input;
	int has_agc_output;
	int thread_created;
	int exit_requested;
	int pending;
	int busy;
} SNAPSHOT_WRITER;

/* Protected by g_snapshot_writer.lock; job owns input_buffer until written. */
static struct {
	int active, job, type, phase;
	int transition_pending, restore_pending, initial_bpr, control_busy;
	UINT64 started_ms;
	unsigned long long phase_saved;
	RAW_RECORDING_LOG metadata;
	unsigned long long target_frames;
	unsigned long long saved, skipped, sequence;
	char directory[192], path[320];
	const char *reason;
} g_raw_recording;

/* Serialized with snapshot buffer ownership by g_snapshot_writer.lock. */
static TIMED_SNAPSHOT g_timed_snapshot;
static MINUTE_RAW_RECORDING g_minute_raw;

static LIVEVIEW_CONTEXT g_liveview;
static UINT32 g_snapshot_count = 0;
static SNAPSHOT_WRITER g_snapshot_writer = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.cond = PTHREAD_COND_INITIALIZER,
};
static pthread_mutex_t g_liveview_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_liveview_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_shutter_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_manual_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ffc_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ffc_test_cond = PTHREAD_COND_INITIALIZER;
static int g_liveview_shutter_fault = 0;
static int g_liveview_stop_request = 0;
static int g_liveview_fault_stop_worker_active = 0;
static int g_ffc_test_pending = 0;
static int g_ffc_test_running = 0;
static int g_ffc_test_worker_active = 0;
static int g_ffc_test_result = -1;
/* Protected by g_ffc_test_lock; only CFG enable requests auto-apply a test. */
static int g_ffc_enable_after_test = 0;
static pthread_mutex_t g_isp_step_test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_isp_step_test_cond = PTHREAD_COND_INITIALIZER;
static int g_isp_step_test_pending = 0;
static int g_isp_step_test_running = 0;
static int g_isp_step_test_result = -1;
static int g_isp_step_test_expected_valid = 0;
static UINT32 g_isp_step_test_run_id = 0;
static UINT32 g_isp_step_test_min_raw_dn = 0U;
static UINT32 g_isp_step_test_max_raw_dn = ISP_RAW_TO_Y_STEP_RAW14_MAX;
static UINT64 g_isp_step_test_expected_timestamp = 0;
static UINT64 g_isp_step_test_expected_count = 0;

static void liveview_stop_pipeline(LIVEVIEW_CONTEXT *ctx);

static int liveview_request_ffc_test(void)
{
	int result;

	pthread_mutex_lock(&g_ffc_test_lock);
	if (!g_liveview.running || !g_ffc_test_worker_active) {
		printf("Run FFC test unavailable: Liveview pipeline is not running\n");
		pthread_mutex_unlock(&g_ffc_test_lock);
		return -1;
	}
	if (g_ffc_test_pending || g_ffc_test_running) {
		printf("Run FFC test is already pending\n");
		pthread_mutex_unlock(&g_ffc_test_lock);
		return -1;
	}

	g_ffc_test_result = -1;
	g_ffc_test_pending = 1;
	while (g_ffc_test_pending || g_ffc_test_running) {
		pthread_cond_wait(&g_ffc_test_cond, &g_ffc_test_lock);
	}
	result = g_ffc_test_result;
	pthread_mutex_unlock(&g_ffc_test_lock);

	return result;
}

static int liveview_take_ffc_test_request(void)
{
	int requested = 0;

	pthread_mutex_lock(&g_ffc_test_lock);
	if (g_ffc_test_pending) {
		g_ffc_test_pending = 0;
		g_ffc_test_running = 1;
		requested = 1;
	}
	pthread_mutex_unlock(&g_ffc_test_lock);

	return requested;
}

static void liveview_finish_ffc_test(int result)
{
	pthread_mutex_lock(&g_shutter_calibration_lock);
	pthread_mutex_lock(&g_manual_calibration_lock);
	pthread_mutex_lock(&g_ffc_test_lock);
	if (g_ffc_test_pending || g_ffc_test_running) {
		if (g_ffc_enable_after_test) {
			int ready = result == 0 && ffc_table_is_ready();
			int requested = g_ffc_enable_after_test > 0;
			if (!ready)
				ffc_init_table();
			ffc_set_enabled(ready && requested);
			g_shutter_reschedule_after_manual_calibration = 1;
			if (!ready)
				printf("FFC enable calibration failed; compensation remains off\n");
			else
				printf(requested ? "FFC calibration complete; compensation applied\n" :
				       "FFC calibration complete; compensation remains off as requested\n");
		}
		g_ffc_enable_after_test = 0;
		g_ffc_test_pending = 0;
		g_ffc_test_running = 0;
		g_ffc_test_result = result;
		pthread_cond_broadcast(&g_ffc_test_cond);
	}
	pthread_mutex_unlock(&g_ffc_test_lock);
	pthread_mutex_unlock(&g_manual_calibration_lock);
	pthread_mutex_unlock(&g_shutter_calibration_lock);
}

static void liveview_set_ffc_test_worker_active(int active)
{
	pthread_mutex_lock(&g_ffc_test_lock);
	g_ffc_test_worker_active = active ? 1 : 0;
	if (!g_ffc_test_worker_active &&
	    (g_ffc_test_pending || g_ffc_test_running)) {
		g_ffc_test_pending = 0;
		g_ffc_test_running = 0;
		g_ffc_test_result = -1;
		g_ffc_enable_after_test = 0;
		pthread_cond_broadcast(&g_ffc_test_cond);
	}
	pthread_mutex_unlock(&g_ffc_test_lock);
}

static int liveview_run_isp_raw_to_y_step_diagnostic_range(UINT32 min_raw_dn,
						      UINT32 max_raw_dn)
{
	struct timespec deadline;
	UINT32 run_id;
	int wait_ret = 0;
	int result;
	int calibration_active;
	int ffc_test_active;

	if (!liveview_is_running()) {
		printf("ISP RAW-to-Y Step Test unavailable: Liveview is stopped\n");
		return -1;
	}
	if (!agc_histogram_p1_p99_is_enabled()) {
		printf("ISP RAW-to-Y Step Test unavailable: enable Histogram P1/P99 AGC (CPU) and restart Liveview first\n");
		return -1;
	}

	pthread_mutex_lock(&g_manual_calibration_lock);
	calibration_active = g_manual_calibration_active;
	pthread_mutex_unlock(&g_manual_calibration_lock);
	if (calibration_active) {
		printf("ISP RAW-to-Y Step Test unavailable: calibration is active\n");
		return -1;
	}

	pthread_mutex_lock(&g_ffc_test_lock);
	ffc_test_active = g_ffc_test_pending || g_ffc_test_running;
	pthread_mutex_unlock(&g_ffc_test_lock);
	if (ffc_test_active) {
		printf("ISP RAW-to-Y Step Test unavailable: FFC test is active\n");
		return -1;
	}
	if (g_yuv_stats_pending) {
		printf("ISP RAW-to-Y Step Test unavailable: YUV diagnostic is pending\n");
		return -1;
	}
	if (isp_raw_to_y_step_diagnostic_get_next_run_id(&run_id) != 0) {
		return -1;
	}
	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		printf("ISP RAW-to-Y Step Test: get timeout clock fail\n");
		return -1;
	}
	deadline.tv_sec += ISP_STEP_TEST_WAIT_TIMEOUT_SEC;

	pthread_mutex_lock(&g_isp_step_test_lock);
	if (g_isp_step_test_pending || g_isp_step_test_running) {
		printf("ISP RAW-to-Y Step Test is already pending\n");
		pthread_mutex_unlock(&g_isp_step_test_lock);
		return -1;
	}
	g_isp_step_test_run_id = run_id;
	g_isp_step_test_min_raw_dn = min_raw_dn;
	g_isp_step_test_max_raw_dn = max_raw_dn;
	g_isp_step_test_result = -1;
	g_isp_step_test_expected_valid = 0;
	g_isp_step_test_pending = 1;
	printf("ISP RAW-to-Y Step Test %04u requested (input %u..%u); "
	       "waiting for matching VideoProc output\n",
	       run_id, min_raw_dn, max_raw_dn);
	while ((g_isp_step_test_pending || g_isp_step_test_running) &&
	       wait_ret == 0) {
		wait_ret = pthread_cond_timedwait(&g_isp_step_test_cond,
			&g_isp_step_test_lock, &deadline);
	}
	if (g_isp_step_test_pending || g_isp_step_test_running) {
		g_isp_step_test_pending = 0;
		g_isp_step_test_running = 0;
		g_isp_step_test_expected_valid = 0;
		g_isp_step_test_result = -1;
		printf("ISP RAW-to-Y Step Test %04u timeout; matching timestamp/count was not received\n",
		       run_id);
	}
	result = g_isp_step_test_result;
	pthread_mutex_unlock(&g_isp_step_test_lock);

	return result;
}

int liveview_run_isp_raw_to_y_step_diagnostic(void)
{
	return liveview_run_isp_raw_to_y_step_diagnostic_range(
		0U, ISP_RAW_TO_Y_STEP_RAW14_MAX);
}

int liveview_run_isp_raw12_to_y_step_diagnostic(void)
{
	return liveview_run_isp_raw_to_y_step_diagnostic_range(
		0U, ISP_RAW_TO_Y_STEP_RAW12_MAX);
}

int liveview_run_isp_raw14_fine_step_diagnostic(void)
{
	return liveview_run_isp_raw_to_y_step_diagnostic_range(
		ISP_RAW_TO_Y_STEP_FINE_MIN, ISP_RAW_TO_Y_STEP_FINE_MAX);
}

static int liveview_take_isp_step_test_request(UINT32 *run_id,
					       UINT32 *min_raw_dn,
					       UINT32 *max_raw_dn)
{
	int requested = 0;

	if (run_id == NULL || min_raw_dn == NULL || max_raw_dn == NULL) {
		return 0;
	}
	pthread_mutex_lock(&g_isp_step_test_lock);
	if (g_isp_step_test_pending) {
		g_isp_step_test_pending = 0;
		g_isp_step_test_running = 1;
		*run_id = g_isp_step_test_run_id;
		*min_raw_dn = g_isp_step_test_min_raw_dn;
		*max_raw_dn = g_isp_step_test_max_raw_dn;
		requested = 1;
	}
	pthread_mutex_unlock(&g_isp_step_test_lock);

	return requested;
}

static void liveview_set_isp_step_test_expected(UINT32 run_id,
						 UINT64 timestamp,
						 UINT64 count)
{
	pthread_mutex_lock(&g_isp_step_test_lock);
	if (g_isp_step_test_running && g_isp_step_test_run_id == run_id) {
		g_isp_step_test_expected_timestamp = timestamp;
		g_isp_step_test_expected_count = count;
		g_isp_step_test_expected_valid =
			(timestamp != 0U || count != 0U) ? 1 : 0;
		printf("ISP RAW-to-Y Step Test %04u input submitted: timestamp=%llu, count=%llu\n",
		       run_id, (unsigned long long)timestamp,
		       (unsigned long long)count);
	}
	pthread_mutex_unlock(&g_isp_step_test_lock);
}

static int liveview_match_isp_step_test_output(
	const HD_VIDEO_FRAME *frame, UINT32 *run_id,
	UINT32 *min_raw_dn, UINT32 *max_raw_dn)
{
	int matched = 0;

	if (frame == NULL || run_id == NULL || min_raw_dn == NULL ||
	    max_raw_dn == NULL) {
		return 0;
	}
	pthread_mutex_lock(&g_isp_step_test_lock);
	if (g_isp_step_test_running && g_isp_step_test_expected_valid &&
	    ((g_isp_step_test_expected_timestamp != 0U &&
	      frame->timestamp == g_isp_step_test_expected_timestamp) ||
	     (frame->timestamp == 0U &&
	      g_isp_step_test_expected_count != 0U &&
	      frame->count == g_isp_step_test_expected_count))) {
		*run_id = g_isp_step_test_run_id;
		*min_raw_dn = g_isp_step_test_min_raw_dn;
		*max_raw_dn = g_isp_step_test_max_raw_dn;
		matched = 1;
		g_isp_step_test_expected_valid = 0;
	}
	pthread_mutex_unlock(&g_isp_step_test_lock);

	return matched;
}

static void liveview_finish_isp_step_test(int result)
{
	pthread_mutex_lock(&g_isp_step_test_lock);
	if (g_isp_step_test_pending || g_isp_step_test_running) {
		g_isp_step_test_pending = 0;
		g_isp_step_test_running = 0;
		g_isp_step_test_expected_valid = 0;
		g_isp_step_test_result = result;
		pthread_cond_broadcast(&g_isp_step_test_cond);
	}
	pthread_mutex_unlock(&g_isp_step_test_lock);
}

static void liveview_set_shutter_fault(int fault)
{
	pthread_mutex_lock(&g_liveview_fault_lock);
	g_liveview_shutter_fault = fault ? 1 : 0;
	pthread_mutex_unlock(&g_liveview_fault_lock);
}

static int liveview_stop_is_requested(void)
{
	int requested;

	pthread_mutex_lock(&g_liveview_fault_lock);
	requested = g_liveview_stop_request;
	pthread_mutex_unlock(&g_liveview_fault_lock);
	return requested;
}

static void *liveview_fault_stop_worker(void *arg)
{
	(void)arg;

	printf("Shutter fault: stopping liveview\n");
	liveview_stop();

	pthread_mutex_lock(&g_liveview_fault_lock);
	g_liveview_stop_request = 0;
	g_liveview_fault_stop_worker_active = 0;
	pthread_mutex_unlock(&g_liveview_fault_lock);
	return 0;
}

static void liveview_request_shutter_fault_stop(VIDEO_LIVEVIEW *stream)
{
	pthread_t stop_thread_id;
	int ret;
	int create_worker = 0;

	pthread_mutex_lock(&g_liveview_fault_lock);
	g_liveview_shutter_fault = 1;
	g_liveview_stop_request = 1;
	if (!g_liveview_fault_stop_worker_active) {
		g_liveview_fault_stop_worker_active = 1;
		create_worker = 1;
	}
	pthread_mutex_unlock(&g_liveview_fault_lock);

	stream->cap_exit = 1;

	if (!create_worker) {
		return;
	}

	ret = pthread_create(&stop_thread_id, NULL,
			     liveview_fault_stop_worker, NULL);
	if (ret != 0) {
		printf("create shutter fault stop worker failed=%d\n", ret);
		pthread_mutex_lock(&g_liveview_fault_lock);
		g_liveview_fault_stop_worker_active = 0;
		pthread_mutex_unlock(&g_liveview_fault_lock);
		return;
	}
	pthread_detach(stop_thread_id);
}

static void liveview_on_shutter_failure(void)
{
	liveview_request_shutter_fault_stop(&g_liveview.stream[0]);
}

static int liveview_manual_shutter_command(int open)
{
	int ret;

	if (pthread_mutex_trylock(&g_shutter_calibration_lock) != 0) {
		printf("Shutter command blocked: shutter-closed calibration is active\n");
		return -1;
	}

	if (open) {
		ret = gpio_shutter_open();
		if (ret == 0) {
			liveview_set_shutter_fault(0);
		}
	} else {
		ret = gpio_shutter_close();
	}

	pthread_mutex_unlock(&g_shutter_calibration_lock);
	return ret;
}

static void liveview_on_manual_calibration_state(const char *name, int active)
{
	int shutter_calibration_enabled;
	const char *calibration_name = name != NULL ? name : "manual";

	pthread_mutex_lock(&g_shutter_calibration_lock);
	pthread_mutex_lock(&g_manual_calibration_lock);
	shutter_calibration_enabled =
		ffc_is_enabled() && ffc_get_shutter_interval_sec() > 0;
	if (active) {
		g_shutter_reschedule_after_manual_calibration = 0;
		g_manual_calibration_active = 1;
		g_nuc_snapshot_pending = 0;
		if (shutter_calibration_enabled) {
			printf("Automatic shutter calibration paused for %s calibration\n",
			       calibration_name);
		}
	} else {
		g_shutter_reschedule_after_manual_calibration = 1;
		g_manual_calibration_active = 0;
		if (shutter_calibration_enabled) {
			printf("Automatic shutter calibration resumed after %s calibration\n",
			       calibration_name);
		}
	}
	pthread_mutex_unlock(&g_manual_calibration_lock);
	pthread_mutex_unlock(&g_shutter_calibration_lock);
}

static int liveview_prepare_shutter_interval(int *enabled)
{
	int paused;

	pthread_mutex_lock(&g_shutter_calibration_lock);
	*enabled = ffc_is_enabled() && ffc_get_shutter_interval_sec() > 0;
	if (g_shutter_reschedule_after_manual_calibration) {
		if (*enabled) {
			liveview_schedule_next_shutter_calibration();
		} else {
			g_shutter_next_calibration_ms = 0;
		}
		g_shutter_reschedule_after_manual_calibration = 0;
	}
	pthread_mutex_lock(&g_manual_calibration_lock);
	paused = g_manual_calibration_active;
	pthread_mutex_unlock(&g_manual_calibration_lock);
	pthread_mutex_unlock(&g_shutter_calibration_lock);

	return paused;
}

static int copy_active_raw_frame(const HD_VIDEO_FRAME *p_video_frame,
	unsigned char *dst, size_t dst_size)
{
	UINTPTR blk_pa;
	UINTPTR map_va;
	UINTPTR raw_va;
	UINT32 stride;
	UINT32 row_bytes = RAW_ACTIVE_W * 2;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 y;

	if (p_video_frame == NULL || dst == NULL ||
	    dst_size < SNAPSHOT_FRAME_BYTES || p_video_frame->blk == 0) {
		return -1;
	}
	/* HDAL loff is normally bytes. Keep compatibility with a pixel-count loff. */
	stride = p_video_frame->loff[0];
	if (stride < p_video_frame->dim.w * 2) {
		stride *= 2;
	}
	if (stride < RAW_SIZE_W * sizeof(UINT16)) {
		printf("snapshot invalid line offset: %u\n", stride);
		return -1;
	}

	blk_pa = hd_common_mem_blk2pa(p_video_frame->blk);
	if (blk_pa == 0 || p_video_frame->phy_addr[0] < blk_pa) {
		printf("snapshot blk2pa error\n");
		return -1;
	}

	plane_height = p_video_frame->ph[0] != 0 ?
		p_video_frame->ph[0] : p_video_frame->dim.h;
	if (plane_height < RAW_SIZE_H) {
		printf("snapshot RAW plane is too short: %u\n", plane_height);
		return -1;
	}
	map_size = (UINT32)(p_video_frame->phy_addr[0] - blk_pa) +
		stride * plane_height;
	map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE,
		blk_pa, map_size);
	if (map_va == 0) {
		printf("snapshot mmap error\n");
		return -1;
	}

	hd_common_mem_flush_cache((void *)map_va, map_size);
	raw_va = map_va + (p_video_frame->phy_addr[0] - blk_pa) +
		RAW_ACTIVE_Y * stride + RAW_ACTIVE_X * 2;

	for (y = 0; y < RAW_ACTIVE_H; y++) {
		memcpy(dst + (size_t)y * row_bytes,
		       (void *)(raw_va + y * stride), row_bytes);
	}
	hd_common_mem_munmap((void *)map_va, map_size);
	return 0;
}

static int save_raw_buffer_internal(const unsigned char *raw_buffer,
	size_t raw_size, const char *file_path, int quiet)
{
	FILE *fp = NULL;
	size_t write_size;
	int ret = -1;

	if (raw_buffer == NULL || raw_size == 0 || file_path == NULL) {
		return -1;
	}

	fp = fopen(file_path, "wb");
	if (fp == NULL) {
		printf("snapshot open %s fail\n", file_path);
		goto exit;
	}

	write_size = fwrite(raw_buffer, 1, raw_size, fp);
	if (write_size != raw_size || ferror(fp)) {
		printf("snapshot write %s fail, size=%u expected=%u\n",
		       file_path, (unsigned int)write_size,
		       (unsigned int)raw_size);
		goto exit;
	}

	if (fflush(fp) != 0) {
		printf("snapshot flush %s fail\n", file_path);
		goto exit;
	}

#if defined(__LINUX)
	/* Make each snapshot durable without requiring a later SD-card unmount. */
	if (fsync(fileno(fp)) != 0) {
		printf("snapshot sync %s fail: %s\n", file_path, strerror(errno));
		goto exit;
	}
#endif

	if (fclose(fp) != 0) {
		fp = NULL;
		printf("snapshot close %s fail\n", file_path);
		goto exit;
	}
	/* Avoid closing the stream again in the common cleanup path. */
	fp = NULL;
	if (!quiet) printf("snapshot saved: %s (%ux%u, 16-bit container)\n",
	       file_path, RAW_ACTIVE_W, RAW_ACTIVE_H);
	ret = 0;

exit:
	if (fp != NULL) {
		fclose(fp);
	}
	return ret;
}

static int save_minute_raw_buffer(const unsigned char *raw, size_t bytes, const char *path)
{
	return save_raw_buffer_internal(raw, bytes, path, 1);
}

static int save_y8_diagnostic_buffer(const unsigned char *y_buffer,
	size_t y_size, const char *file_path)
{
	FILE *fp = NULL;
	size_t write_size;
	int ret = -1;

	if (y_buffer == NULL || y_size == 0 || file_path == NULL) {
		return -1;
	}

	fp = fopen(file_path, "wb");
	if (fp == NULL) {
		printf("VideoProc Y8 diagnostic open %s fail\n", file_path);
		goto exit;
	}
	write_size = fwrite(y_buffer, 1, y_size, fp);
	if (write_size != y_size || ferror(fp)) {
		printf("VideoProc Y8 diagnostic write %s fail, size=%u expected=%u\n",
		       file_path, (unsigned int)write_size,
		       (unsigned int)y_size);
		goto exit;
	}
	if (fflush(fp) != 0) {
		printf("VideoProc Y8 diagnostic flush %s fail\n", file_path);
		goto exit;
	}
#if defined(__LINUX)
	if (fsync(fileno(fp)) != 0) {
		printf("VideoProc Y8 diagnostic sync %s fail: %s\n",
		       file_path, strerror(errno));
		goto exit;
	}
#endif
	if (fclose(fp) != 0) {
		fp = NULL;
		printf("VideoProc Y8 diagnostic close %s fail\n", file_path);
		goto exit;
	}
	fp = NULL;
	printf("VideoProc Y8 diagnostic saved: %s (%u bytes)\n",
	       file_path, (unsigned int)y_size);
	ret = 0;

exit:
	if (fp != NULL) {
		fclose(fp);
	}
	return ret;
}

static UINT8 yuv_histogram_percentile(const UINT64 histogram[256],
	UINT64 pixel_count, UINT32 percentile)
{
	UINT64 target_index;
	UINT64 cumulative = 0;
	UINT32 value;

	if (pixel_count == 0) {
		return 0;
	}
	target_index = ((pixel_count - 1U) * percentile) / 100U;
	for (value = 0; value < 256U; value++) {
		cumulative += histogram[value];
		if (cumulative > target_index) {
			return (UINT8)value;
		}
	}
	return 255U;
}

static void print_requested_yuv_frame_stats(const HD_VIDEO_FRAME *p_video_frame)
{
	UINTPTR blk_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR y_va;
	UINT32 data_offset;
	UINT32 stride;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 width;
	UINT32 height;
	UINT32 x;
	UINT32 y;
	UINT32 value;
	UINT8 min_value = 0xFF;
	UINT8 max_value = 0;
	UINT8 p1;
	UINT8 p50;
	UINT8 p99;
	UINT64 sum = 0;
	UINT64 pixel_count;
	UINT64 histogram[256] = {0};
	UINT64 limited_black_count = 0;
	UINT64 limited_white_count = 0;
	UINT32 unique_count = 0;
	unsigned char *y_buffer = NULL;
	char file_path[192];

	if (!g_yuv_stats_pending) {
		return;
	}
	if (p_video_frame == NULL || p_video_frame->phy_addr[0] == 0) {
		printf("YUV diagnostic: frame or Y-plane address is invalid\n");
		g_yuv_stats_pending = 0;
		return;
	}

	width = p_video_frame->dim.w;
	height = p_video_frame->dim.h;
	stride = p_video_frame->loff[0];
	plane_height = p_video_frame->ph[0] != 0 ?
		p_video_frame->ph[0] : height;
	printf("YUV diagnostic frame: blk=0x%llX, pa=0x%llX, "
	       "fmt=0x%08X, dim=%ux%u, loff=%u, ph=%u\n",
	       (unsigned long long)p_video_frame->blk,
	       (unsigned long long)p_video_frame->phy_addr[0],
	       p_video_frame->pxlfmt, width, height, stride, plane_height);
	if (width == 0 || height == 0 || plane_height < height || stride < width) {
		printf("YUV stats invalid Y plane: %ux%u, stride=%u, ph=%u\n",
		       width, height, stride, plane_height);
		g_yuv_stats_pending = 0;
		return;
	}

	blk_pa = p_video_frame->blk != 0 ?
		hd_common_mem_blk2pa(p_video_frame->blk) : 0;
	if (blk_pa != 0 && p_video_frame->phy_addr[0] >= blk_pa) {
		map_pa = blk_pa;
		data_offset = (UINT32)(p_video_frame->phy_addr[0] - blk_pa);
	} else {
		map_pa = p_video_frame->phy_addr[0];
		data_offset = 0;
	}

	map_size = data_offset + stride * plane_height;
	map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE,
		map_pa, map_size);
	if (map_va == 0) {
		printf("YUV stats mmap error\n");
		g_yuv_stats_pending = 0;
		return;
	}

	y_buffer = (unsigned char *)malloc((size_t)width * height);
	if (y_buffer == NULL) {
		printf("YUV diagnostic buffer allocation failed; statistics only\n");
	}
	hd_common_mem_flush_cache((void *)map_va, map_size);
	y_va = map_va + data_offset;
	for (y = 0; y < height; y++) {
		const UINT8 *row = (const UINT8 *)(y_va + y * stride);
		if (y_buffer != NULL) {
			memcpy(y_buffer + (size_t)y * width, row, width);
		}

		for (x = 0; x < width; x++) {
			UINT8 y_value = row[x];

			if (y_value < min_value) min_value = y_value;
			if (y_value > max_value) max_value = y_value;
			sum += y_value;
			histogram[y_value]++;
		}
	}
	pixel_count = (UINT64)width * height;
	p1 = yuv_histogram_percentile(histogram, pixel_count, 1U);
	p50 = yuv_histogram_percentile(histogram, pixel_count, 50U);
	p99 = yuv_histogram_percentile(histogram, pixel_count, 99U);
	for (value = 0; value < 256U; value++) {
		if (histogram[value] != 0) {
			unique_count++;
		}
		if (value <= 16U) {
			limited_black_count += histogram[value];
		}
		if (value >= 235U) {
			limited_white_count += histogram[value];
		}
	}
	printf("Y plane stats: min=%u, max=%u, mean=%.4f, pixels=%llu\n",
	       min_value, max_value,
	       (double)sum / (double)pixel_count,
	       (unsigned long long)pixel_count);
	printf("Y plane distribution: P1=%u, P50=%u, P99=%u, unique=%u\n",
	       p1, p50, p99, unique_count);
	printf("Y plane clipping: zero=%llu (%.4f%%), full255=%llu (%.4f%%)\n",
	       (unsigned long long)histogram[0],
	       100.0 * (double)histogram[0] / (double)pixel_count,
	       (unsigned long long)histogram[255],
	       100.0 * (double)histogram[255] / (double)pixel_count);
	printf("Y limited-range risk: Y<=16=%llu (%.4f%%), "
	       "Y>=235=%llu (%.4f%%)\n",
	       (unsigned long long)limited_black_count,
	       100.0 * (double)limited_black_count / (double)pixel_count,
	       (unsigned long long)limited_white_count,
	       100.0 * (double)limited_white_count / (double)pixel_count);

	hd_common_mem_munmap((void *)map_va, map_size);
	if (y_buffer != NULL) {
		snprintf(file_path, sizeof(file_path),
			 DISPLAY_DIAG_DIR "/display_diag_%04u_videoproc_y8_%ux%u.raw",
			 g_yuv_diag_id, width, height);
		save_y8_diagnostic_buffer(y_buffer,
			(size_t)width * height, file_path);
		free(y_buffer);
	}
	g_yuv_stats_pending = 0;
}

static void process_requested_isp_step_test_output(
	VIDEO_LIVEVIEW *p_stream, const HD_VIDEO_FRAME *frame)
{
	UINT32 run_id;
	UINT32 min_raw_dn;
	UINT32 max_raw_dn;
	UINTPTR blk_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR y_va;
	UINT32 data_offset;
	UINT32 stride;
	UINT32 plane_height;
	UINT32 map_size;
	int result;

	if (!liveview_match_isp_step_test_output(frame, &run_id,
						 &min_raw_dn, &max_raw_dn)) {
		return;
	}
	printf("ISP RAW-to-Y Step Test %04u matching output: timestamp=%llu, count=%llu\n",
	       run_id, (unsigned long long)frame->timestamp,
	       (unsigned long long)frame->count);
	if (p_stream == NULL || p_stream->thermal_isp_step_test_va == 0 ||
	    frame->phy_addr[0] == 0 || frame->dim.w != RAW_SIZE_W ||
	    frame->dim.h != RAW_SIZE_H) {
		printf("ISP RAW-to-Y Step Test: invalid test buffer or output frame\n");
		liveview_finish_isp_step_test(-1);
		return;
	}

	stride = frame->loff[0];
	plane_height = frame->ph[0] != 0U ? frame->ph[0] : frame->dim.h;
	if (stride < frame->dim.w || plane_height < frame->dim.h) {
		printf("ISP RAW-to-Y Step Test: invalid Y plane stride=%u, ph=%u\n",
		       stride, plane_height);
		liveview_finish_isp_step_test(-1);
		return;
	}

	blk_pa = frame->blk != 0 ? hd_common_mem_blk2pa(frame->blk) : 0;
	if (blk_pa != 0 && frame->phy_addr[0] >= blk_pa) {
		map_pa = blk_pa;
		data_offset = (UINT32)(frame->phy_addr[0] - blk_pa);
	} else {
		map_pa = frame->phy_addr[0];
		data_offset = 0;
	}
	map_size = data_offset + stride * plane_height;
	map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE,
		map_pa, map_size);
	if (map_va == 0) {
		printf("ISP RAW-to-Y Step Test: mmap output Y fail\n");
		liveview_finish_isp_step_test(-1);
		return;
	}
	hd_common_mem_flush_cache((void *)map_va, map_size);
	y_va = map_va + data_offset;
	result = isp_raw_to_y_step_diagnostic_save_and_analyze(
		run_id,
		min_raw_dn,
		max_raw_dn,
		(const uint16_t *)p_stream->thermal_isp_step_test_va,
		RAW_SIZE_W * sizeof(uint16_t),
		(const uint8_t *)y_va,
		stride,
		frame->dim.w,
		frame->dim.h);
	if (hd_common_mem_munmap((void *)map_va, map_size) != HD_OK) {
		printf("ISP RAW-to-Y Step Test: munmap output Y fail\n");
		result = -1;
	}
	liveview_finish_isp_step_test(result);
}

static int snapshot_get_next_id(UINT32 *p_snapshot_id)
{
	if (p_snapshot_id == NULL) {
		return -1;
	}

#if defined(__LINUX)
	{
		DIR *dir;
		struct dirent *entry;
		UINT32 max_snapshot_id = 0;
		int snapshot_found = 0;
		const size_t prefix_len = sizeof(SNAPSHOT_PREFIX) - 1U;

		dir = opendir(RAW_SNAPSHOT_DIR);
		if (dir == NULL) {
			printf("snapshot scan %s fail\n", RAW_SNAPSHOT_DIR);
			return -1;
		}

		while ((entry = readdir(dir)) != NULL) {
			char *number_end;
			unsigned long long existing_id;

			if (strncmp(entry->d_name, SNAPSHOT_PREFIX, prefix_len) != 0) {
				continue;
			}

			errno = 0;
			existing_id = strtoull(entry->d_name + prefix_len,
					       &number_end, 10);
			if (errno == ERANGE ||
			    number_end == entry->d_name + prefix_len ||
			    *number_end != '_' || existing_id > UINT_MAX) {
				continue;
			}

			if (!snapshot_found || existing_id > max_snapshot_id) {
				max_snapshot_id = (UINT32)existing_id;
				snapshot_found = 1;
			}
		}
		closedir(dir);

		if (snapshot_found && max_snapshot_id == UINT_MAX) {
			printf("snapshot sequence is exhausted\n");
			return -1;
		}
		if (snapshot_found && g_snapshot_count <= max_snapshot_id) {
			g_snapshot_count = max_snapshot_id + 1U;
			printf("snapshot sequence continues at %u\n",
			       g_snapshot_count);
		}
	}
#endif

	if (g_snapshot_count == UINT_MAX) {
		printf("snapshot sequence is exhausted\n");
		return -1;
	}
	*p_snapshot_id = g_snapshot_count++;
	return 0;
}

static int display_diag_get_next_id(UINT32 *p_diag_id)
{
	if (p_diag_id == NULL) {
		return -1;
	}

#if defined(__LINUX)
	{
		DIR *dir;
		struct dirent *entry;
		UINT32 max_diag_id = 0;
		int diag_found = 0;
		const size_t prefix_len = sizeof(DISPLAY_DIAG_PREFIX) - 1U;

		dir = opendir(DISPLAY_DIAG_DIR);
		if (dir == NULL) {
			printf("display diagnostic scan %s fail\n",
			       DISPLAY_DIAG_DIR);
			return -1;
		}

		while ((entry = readdir(dir)) != NULL) {
			char *number_end;
			unsigned long long existing_id;

			if (strncmp(entry->d_name, DISPLAY_DIAG_PREFIX,
				    prefix_len) != 0) {
				continue;
			}

			errno = 0;
			existing_id = strtoull(entry->d_name + prefix_len,
					       &number_end, 10);
			if (errno == ERANGE ||
			    number_end == entry->d_name + prefix_len ||
			    *number_end != '_' || existing_id > UINT_MAX) {
				continue;
			}
			if (!diag_found || existing_id > max_diag_id) {
				max_diag_id = (UINT32)existing_id;
				diag_found = 1;
			}
		}
		closedir(dir);

		if (diag_found && max_diag_id == UINT_MAX) {
			printf("display diagnostic sequence is exhausted\n");
			return -1;
		}
		if (diag_found && g_display_diag_count <= max_diag_id) {
			g_display_diag_count = max_diag_id + 1U;
			printf("display diagnostic sequence continues at %u\n",
			       g_display_diag_count);
		}
	}
#endif

	if (g_display_diag_count == UINT_MAX) {
		printf("display diagnostic sequence is exhausted\n");
		return -1;
	}
	*p_diag_id = g_display_diag_count++;
	return 0;
}

static int save_raw_buffer(const unsigned char *raw_buffer,
	size_t raw_size, const char *file_path)
{
	return save_raw_buffer_internal(raw_buffer, raw_size, file_path, 0);
}

static int snapshot_write_job(UINT32 snapshot_id, int final_is_input,
			      int has_agc_output)
{
	char file_path[160];
	int ret = 0;

	if (final_is_input) {
		snprintf(file_path, sizeof(file_path),
			 RAW_SNAPSHOT_DIR "/snapshot_%04u_raw16_%ux%u.raw",
			 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
		if (save_raw_buffer(g_snapshot_writer.input_buffer,
				    SNAPSHOT_FRAME_BYTES, file_path) != 0) {
			ret = -1;
		}
	} else {
		snprintf(file_path, sizeof(file_path),
			 RAW_SNAPSHOT_DIR "/snapshot_%04u_input_raw16_%ux%u.raw",
			 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
		if (save_raw_buffer(g_snapshot_writer.input_buffer,
				    SNAPSHOT_FRAME_BYTES, file_path) != 0) {
			ret = -1;
		}

		snprintf(file_path, sizeof(file_path),
			 RAW_SNAPSHOT_DIR "/snapshot_%04u_corrected_raw16_%ux%u.raw",
			 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
		if (save_raw_buffer(g_snapshot_writer.corrected_buffer,
				    SNAPSHOT_FRAME_BYTES, file_path) != 0) {
			ret = -1;
		}
	}

	if (has_agc_output) {
		snprintf(file_path, sizeof(file_path),
			 RAW_SNAPSHOT_DIR "/snapshot_%04u_agc_output_raw16_%ux%u.raw",
			 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
		if (save_raw_buffer(g_snapshot_writer.agc_output_buffer,
				    SNAPSHOT_FRAME_BYTES, file_path) != 0) {
			ret = -1;
		}
	}

	return ret;
}

static void raw_recording_stop_locked(const char *reason)
{
	if (g_raw_recording.active) {
		g_raw_recording.active = 0;
		g_raw_recording.reason = reason;
		printf("RAW recording stopped: %s; saved=%llu, skipped=%llu%s\n",
			reason, g_raw_recording.saved, g_raw_recording.skipped,
			g_raw_recording.job ? "; finishing current file" : "");
	}
}

static void raw_recording_check_limit_locked(void)
{
	if (!g_raw_recording.active || g_raw_recording.phase_saved < g_raw_recording.target_frames)
		return;
	if (g_raw_recording.type == 3 && g_raw_recording.phase < 2) {
		g_raw_recording.phase++;
		g_raw_recording.phase_saved = 0;
		if (g_raw_recording.phase == 2) g_raw_recording.transition_pending = 1;
		printf("RAW recording: advancing to stage %d/3\n", g_raw_recording.phase + 1);
	} else {
		raw_recording_stop_locked("requested image count reached");
	}
}

static int raw_recording_has_space(const char *path)
{
#if defined(__LINUX)
	struct statvfs info;
	if (statvfs(path, &info) != 0) return 0;
	return (unsigned long long)info.f_bavail * info.f_frsize >=
		256ULL * 1024 * 1024 + SNAPSHOT_FRAME_BYTES;
#else
	(void)path;
	return 0;
#endif
}

/* VCAP owner only, before releasing any source buffer. No disk I/O here. */
static void raw_recording_queue(HD_VIDEO_FRAME *input,
	HD_VIDEO_FRAME *corrected, HD_VIDEO_FRAME *agc, int64_t capture_unix_ms)
{
	HD_VIDEO_FRAME *selected;
	const char *stage;
	const char *filename;
	int type;
	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_check_limit_locked();
	if (!g_raw_recording.active || g_raw_recording.transition_pending ||
	    g_raw_recording.control_busy) goto done;
	if (g_snapshot_writer.busy) {
		g_raw_recording.skipped++;
		goto done;
	}
	type = g_raw_recording.type == 3 ?
		(g_raw_recording.phase == 1 ? 1 : 0) : g_raw_recording.type;
	stage = g_raw_recording.type == 3 && g_raw_recording.phase == 2 ?
		"original_bpr_off" : (type == 0 ? "original" :
		(type == 1 ? "corrected" : "agc_output"));
	selected = type == 0 ? input : (type == 1 ? corrected : agc);
	if (selected == NULL) {
		raw_recording_stop_locked("selected RAW output unavailable");
		goto done;
	}
	g_snapshot_writer.busy = 1;
	if (copy_active_raw_frame(selected, g_snapshot_writer.input_buffer,
	                         SNAPSHOT_FRAME_BYTES) != 0) {
		g_snapshot_writer.busy = 0;
		raw_recording_stop_locked("RAW copy failed");
		goto done;
	}
	memset(&g_raw_recording.metadata, 0, sizeof(g_raw_recording.metadata));
	g_raw_recording.metadata.sequence = g_raw_recording.sequence++;
	g_raw_recording.metadata.phase_frame = g_raw_recording.phase_saved + 1;
	g_raw_recording.metadata.elapsed_ms = liveview_get_time_ms() - g_raw_recording.started_ms;
	g_raw_recording.metadata.capture_unix_ms = capture_unix_ms;
	g_raw_recording.metadata.sensor_temp_valid =
		sensor_temp_read(&g_raw_recording.metadata.sensor_temp_c) == 0;
	g_raw_recording.metadata.bpr_enabled = bpr_is_enabled();
	g_raw_recording.metadata.stage = stage;
	snprintf(g_raw_recording.path, sizeof(g_raw_recording.path),
		"%s/frame_%06llu_t%llums_%s_raw16_%ux%u.raw", g_raw_recording.directory,
		(unsigned long long)g_raw_recording.metadata.sequence,
		(unsigned long long)g_raw_recording.metadata.elapsed_ms, stage,
		RAW_ACTIVE_W, RAW_ACTIVE_H);
	filename = strrchr(g_raw_recording.path, '/');
	g_raw_recording.metadata.raw_file = filename ? filename + 1 : g_raw_recording.path;
	g_raw_recording.job = 1;
	g_snapshot_writer.pending = 1;
	pthread_cond_signal(&g_snapshot_writer.cond);
 done:
	pthread_mutex_unlock(&g_snapshot_writer.lock);
}

static void *snapshot_writer_thread(void *arg)
{
	UINT32 snapshot_id;
	int final_is_input;
	int has_agc_output;
	int write_ret;
	int recording_job;
	int timed_job;
	int minute_job;

	(void)arg;
	for (;;) {
		pthread_mutex_lock(&g_snapshot_writer.lock);
		while (!g_snapshot_writer.pending &&
		       !g_snapshot_writer.exit_requested) {
			pthread_cond_wait(&g_snapshot_writer.cond, &g_snapshot_writer.lock);
		}
		if (!g_snapshot_writer.pending &&
		    g_snapshot_writer.exit_requested) {
			pthread_mutex_unlock(&g_snapshot_writer.lock);
			break;
		}

		recording_job = g_raw_recording.job;
		timed_job = g_timed_snapshot.in_flight;
		minute_job = g_minute_raw.in_flight;
		snapshot_id = g_snapshot_writer.snapshot_id;
		final_is_input = g_snapshot_writer.final_is_input;
		has_agc_output = g_snapshot_writer.has_agc_output;
		g_snapshot_writer.pending = 0;
		pthread_mutex_unlock(&g_snapshot_writer.lock);

		if (recording_job) {
			write_ret = raw_recording_has_space(g_raw_recording.directory) ?
				save_raw_buffer_internal(g_snapshot_writer.input_buffer, SNAPSHOT_FRAME_BYTES,
					g_raw_recording.path, 1) : -1;
			if (write_ret != 0) remove(g_raw_recording.path);
			else
				write_ret = raw_recording_log_append(g_raw_recording.directory,
					&g_raw_recording.metadata);
		} else if (minute_job) {
			write_ret = raw_recording_has_space(g_minute_raw.directory) ?
				minute_raw_save(&g_minute_raw, g_snapshot_writer.input_buffer,
					g_snapshot_writer.corrected_buffer, RAW_ACTIVE_W, RAW_ACTIVE_H,
					save_minute_raw_buffer) : -1;
		} else if (timed_job) {
			write_ret = timed_snapshot_save(&g_timed_snapshot,
				g_snapshot_writer.input_buffer,
				final_is_input ? NULL : g_snapshot_writer.corrected_buffer,
				has_agc_output ? g_snapshot_writer.agc_output_buffer : NULL,
				RAW_ACTIVE_W, RAW_ACTIVE_H, save_raw_buffer);
		} else {
			write_ret = snapshot_write_job(snapshot_id, final_is_input,
						       has_agc_output);
			printf("Snapshot %04u background save %s\n",
			       snapshot_id, write_ret == 0 ? "done" : "failed");
		}

		pthread_mutex_lock(&g_snapshot_writer.lock);
		if (timed_job) timed_snapshot_finish(&g_timed_snapshot, write_ret);
		if (minute_job) minute_raw_finish(&g_minute_raw, write_ret);
		if (recording_job) {
			g_raw_recording.job = 0;
			if (write_ret == 0) {
				g_raw_recording.saved++;
				g_raw_recording.phase_saved++;
			}
			else raw_recording_stop_locked("write failed or insufficient SD space");
			raw_recording_check_limit_locked();
			if (!g_raw_recording.active)
				printf("RAW recording complete: saved=%llu, skipped=%llu\n",
					g_raw_recording.saved, g_raw_recording.skipped);
		}
		g_snapshot_writer.busy = 0;
		pthread_cond_broadcast(&g_snapshot_writer.cond);
		pthread_mutex_unlock(&g_snapshot_writer.lock);
	}

	return NULL;
}

static int snapshot_writer_start(void)
{
	int ret;

	if (g_snapshot_writer.thread_created) {
		return 0;
	}

	g_snapshot_writer.input_buffer =
		(unsigned char *)malloc(SNAPSHOT_FRAME_BYTES);
	g_snapshot_writer.corrected_buffer =
		(unsigned char *)malloc(SNAPSHOT_FRAME_BYTES);
	g_snapshot_writer.agc_output_buffer =
		(unsigned char *)malloc(SNAPSHOT_FRAME_BYTES);
	if (g_snapshot_writer.input_buffer == NULL ||
	    g_snapshot_writer.corrected_buffer == NULL ||
	    g_snapshot_writer.agc_output_buffer == NULL) {
		printf("allocate snapshot writer buffers fail\n");
		free(g_snapshot_writer.agc_output_buffer);
		free(g_snapshot_writer.corrected_buffer);
		free(g_snapshot_writer.input_buffer);
		g_snapshot_writer.agc_output_buffer = NULL;
		g_snapshot_writer.corrected_buffer = NULL;
		g_snapshot_writer.input_buffer = NULL;
		return -1;
	}

	g_snapshot_writer.exit_requested = 0;
	g_snapshot_writer.pending = 0;
	g_snapshot_writer.busy = 0;
	g_snapshot_writer.has_agc_output = 0;
	ret = pthread_create(&g_snapshot_writer.thread_id, NULL,
			     snapshot_writer_thread, NULL);
	if (ret != 0) {
		printf("create snapshot_writer_thread failed=%d\n", ret);
		free(g_snapshot_writer.agc_output_buffer);
		free(g_snapshot_writer.corrected_buffer);
		free(g_snapshot_writer.input_buffer);
		g_snapshot_writer.agc_output_buffer = NULL;
		g_snapshot_writer.corrected_buffer = NULL;
		g_snapshot_writer.input_buffer = NULL;
		return -1;
	}
	g_snapshot_writer.thread_created = 1;
	printf("Snapshot background writer ready\n");
	return 0;
}

static void snapshot_writer_stop(void)
{
	int was_busy;

	if (!g_snapshot_writer.thread_created) {
		return;
	}

	pthread_mutex_lock(&g_snapshot_writer.lock);
	was_busy = g_snapshot_writer.busy;
	g_snapshot_writer.exit_requested = 1;
	pthread_cond_signal(&g_snapshot_writer.cond);
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	if (was_busy) {
		printf("Waiting for background snapshot save to finish...\n");
	}
	pthread_join(g_snapshot_writer.thread_id, NULL);

	pthread_mutex_lock(&g_snapshot_writer.lock);
	g_snapshot_writer.thread_created = 0;
	g_snapshot_writer.exit_requested = 0;
	g_snapshot_writer.pending = 0;
	g_snapshot_writer.busy = 0;
	g_snapshot_writer.has_agc_output = 0;
	pthread_mutex_unlock(&g_snapshot_writer.lock);

	free(g_snapshot_writer.agc_output_buffer);
	free(g_snapshot_writer.corrected_buffer);
	free(g_snapshot_writer.input_buffer);
	g_snapshot_writer.agc_output_buffer = NULL;
	g_snapshot_writer.corrected_buffer = NULL;
	g_snapshot_writer.input_buffer = NULL;
}

static int snapshot_writer_reserve_request(void)
{
	int ret = 0;

	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (!g_snapshot_writer.thread_created ||
	    g_snapshot_writer.exit_requested || g_snapshot_writer.busy) {
		ret = -1;
	} else {
		g_snapshot_writer.busy = 1;
	}
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	return ret;
}

static void snapshot_writer_cancel_request(void)
{
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (!g_snapshot_writer.pending) {
		timed_snapshot_finish(&g_timed_snapshot, -1);
		minute_raw_finish(&g_minute_raw, -1);
		g_snapshot_writer.busy = 0;
	}
	pthread_mutex_unlock(&g_snapshot_writer.lock);
}

static int timed_snapshot_reserve_request(void)
{
	int reserved = 0;
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (g_snapshot_writer.thread_created && !g_snapshot_writer.exit_requested &&
	    !g_snapshot_writer.busy && !g_raw_recording.active && !g_raw_recording.job) {
		if (minute_raw_claim(&g_minute_raw, liveview_get_time_ms()))
			reserved = SCHEDULED_MINUTE_RAW;
		else if (timed_snapshot_claim(&g_timed_snapshot, liveview_get_time_ms()))
			reserved = SCHEDULED_TIMED_SNAPSHOT;
		if (reserved) g_snapshot_writer.busy = 1;
	}
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	return reserved;
}

static int queue_pipeline_snapshot(HD_VIDEO_FRAME *p_input_frame,
	HD_VIDEO_FRAME *p_final_frame, int final_is_input,
	HD_VIDEO_FRAME *p_agc_output_frame, int has_agc_output, int timed_job, int64_t capture_unix_ms)
{
	UINT32 snapshot_id = 0;
	float sensor_temp_c = 0.0f;
	int sensor_temp_valid = 0;

	if (timed_job == SCHEDULED_MINUTE_RAW) has_agc_output = 0;
	if (!timed_job && snapshot_get_next_id(&snapshot_id) != 0) {
		snapshot_writer_cancel_request();
		return -1;
	}
	if (copy_active_raw_frame(p_input_frame,
				  g_snapshot_writer.input_buffer,
				  SNAPSHOT_FRAME_BYTES) != 0) {
		printf("snapshot input frame copy fail\n");
		snapshot_writer_cancel_request();
		return -1;
	}
	if ((!final_is_input || timed_job == SCHEDULED_MINUTE_RAW) &&
	    copy_active_raw_frame(p_final_frame,
				  g_snapshot_writer.corrected_buffer,
				  SNAPSHOT_FRAME_BYTES) != 0) {
		printf("snapshot corrected frame copy fail\n");
		snapshot_writer_cancel_request();
		return -1;
	}
	if (has_agc_output &&
	    copy_active_raw_frame(p_agc_output_frame,
				  g_snapshot_writer.agc_output_buffer,
				  SNAPSHOT_FRAME_BYTES) != 0) {
		printf("snapshot AGC output frame copy fail\n");
		snapshot_writer_cancel_request();
		return -1;
	}

	/* Read alongside the copied frame, before publishing to the SD writer.
	 * Use the same fresh reading as Show Current Sensor Temperature. */
	if (timed_job) {
		sensor_temp_valid = sensor_temp_read(&sensor_temp_c) == 0;
	}

	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (!g_snapshot_writer.thread_created ||
	    g_snapshot_writer.exit_requested) {
		timed_snapshot_finish(&g_timed_snapshot, -1);
		minute_raw_finish(&g_minute_raw, -1);
		g_snapshot_writer.busy = 0;
		pthread_mutex_unlock(&g_snapshot_writer.lock);
		printf("snapshot writer stopped before queue\n");
		return -1;
	}
	g_snapshot_writer.snapshot_id = snapshot_id;
	g_snapshot_writer.final_is_input = final_is_input;
	g_snapshot_writer.has_agc_output = has_agc_output ? 1 : 0;
	g_snapshot_writer.pending = 1;
	if (timed_job == SCHEDULED_MINUTE_RAW) {
		g_minute_raw.captured_unix_ms = capture_unix_ms;
		g_minute_raw.sensor_temp_c = sensor_temp_c;
		g_minute_raw.sensor_temp_valid = sensor_temp_valid;
	} else if (timed_job == SCHEDULED_TIMED_SNAPSHOT) {
		g_timed_snapshot.sensor_temp_c = sensor_temp_c;
		g_timed_snapshot.sensor_temp_valid = sensor_temp_valid;
		printf("Timed Snapshot %u/%u copied; background save queued\n",
			g_timed_snapshot.saved + 1U, TIMED_SNAPSHOT_COUNT);
		if (sensor_temp_valid) {
			printf("Current Sensor Temperature = %.4f C\n", (double)sensor_temp_c);
		} else {
			printf("Current Sensor Temperature read failed; saving snapshot with empty temperature\n");
		}
	}
	pthread_cond_signal(&g_snapshot_writer.cond);
	pthread_mutex_unlock(&g_snapshot_writer.lock);

	if (!timed_job) {
		printf("Snapshot %04u copied; background save queued, AGC output=%s\n",
		       snapshot_id, has_agc_output ? "yes" : "no");
	}
	return 0;
}

static int copy_videoframe_raw(HD_VIDEO_FRAME *p_video_frame, UINTPTR dst_va, UINT32 dst_size)
{
	UINTPTR blk_pa;
	UINTPTR map_va;
	UINTPTR src_va;
	UINT32 stride;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 row_bytes = RAW_SIZE_W * sizeof(UINT16);
	UINT32 required_size = row_bytes * RAW_SIZE_H;
	UINT32 y;

	if (p_video_frame == NULL || p_video_frame->blk == 0 ||
	    dst_va == 0 || dst_size < required_size) {
		printf("copy full raw invalid argument\n");
		return -1;
	}
	stride = p_video_frame->loff[0];
	if (stride < p_video_frame->dim.w * sizeof(UINT16)) {
		stride *= sizeof(UINT16);
	}
	if (stride < row_bytes) {
		printf("copy full raw invalid line offset: %u\n", stride);
		return -1;
	}

	blk_pa = hd_common_mem_blk2pa(p_video_frame->blk);
	if (blk_pa == 0 || p_video_frame->phy_addr[0] < blk_pa) {
		printf("copy raw blk2pa error !!\r\n\r\n");
		return -1;
	}

	plane_height = p_video_frame->ph[0] != 0 ?
		p_video_frame->ph[0] : p_video_frame->dim.h;
	if (plane_height < RAW_SIZE_H) {
		printf("copy full raw plane is too short: %u\n", plane_height);
		return -1;
	}
	map_size = (UINT32)(p_video_frame->phy_addr[0] - blk_pa) +
		stride * plane_height;
	map_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, blk_pa, map_size);
	if (map_va == 0) {
		printf("copy raw mmap error !!\r\n\r\n");
		return -1;
	}

	hd_common_mem_flush_cache((void *)map_va, map_size);
	src_va = map_va + (p_video_frame->phy_addr[0] - blk_pa);
	for (y = 0; y < RAW_SIZE_H; y++) {
		memcpy((void *)(dst_va + y * row_bytes),
		       (void *)(src_va + y * stride), row_bytes);
	}
	hd_common_mem_munmap((void *)map_va, map_size);

	return 0;
}

static int liveview_update_ffc_table_from_average(VIDEO_LIVEVIEW *p_stream,
						   UINT32 lineoffset)
{
	UINT32 blk_size;

	if (p_stream->thermal_ffc_out_va == 0 || lineoffset == 0) {
		printf("FFC average buffer is invalid\n");
		return -1;
	}

	blk_size = VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_out_va, blk_size);

	return ffc_update_table_from_raw(p_stream->thermal_ffc_out_va,
					lineoffset,
					RAW_ACTIVE_X,
					RAW_ACTIVE_Y,
					RAW_ACTIVE_W,
					RAW_ACTIVE_H);
}

static int liveview_run_ffc_gfx_test(VIDEO_LIVEVIEW *p_stream,
				     UINT32 lineoffset)
{
	HD_VIDEO_FRAME average_frame = {0};
	HD_VIDEO_FRAME output_frame = {0};
	UINT32 blk_size =
		VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);
	HD_RESULT ret;

	if (p_stream == NULL || lineoffset == 0 ||
	    p_stream->thermal_ffc_out_pa == 0 ||
	    p_stream->thermal_ffc_out_va == 0 ||
	    p_stream->thermal_ffc_zero_pa == 0 ||			// Tim FFC
	    p_stream->thermal_ffc_zero_va == 0 ||
	    p_stream->thermal_ffc_k_frame_pa == 0 ||
	    p_stream->thermal_ffc_k_frame_va == 0 ||
	    p_stream->thermal_ffc_b_frame_pa == 0 ||
	    p_stream->thermal_ffc_sum_pa == 0 ||
	    p_stream->thermal_ffc_sum_va == 0 ||
	    p_stream->thermal_ffc_sum_blk == 0) {
		printf("Run FFC test buffer is invalid\n");
		return -1;
	}

	/*
	 * The four-frame average ends in thermal_ffc_out.  Use the idle
	 * accumulation buffer as a separate GFX output so input and output do
	 * not alias during this diagnostic pass.
	 */
	memset((void *)p_stream->thermal_ffc_sum_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_sum_va,
				  blk_size);

	average_frame.phy_addr[0] = p_stream->thermal_ffc_out_pa;
	average_frame.blk = p_stream->thermal_ffc_out_blk;
	average_frame.loff[0] = lineoffset;
	average_frame.dim.w = RAW_SIZE_W;
	average_frame.dim.h = RAW_SIZE_H;
	average_frame.ph[0] = RAW_SIZE_H;

	ret = ffc_apply(&average_frame,
			&output_frame,
			p_stream->thermal_ffc_k_frame_pa,
			p_stream->thermal_ffc_b_frame_pa,
			p_stream->thermal_ffc_sum_pa,
			p_stream->thermal_ffc_sum_blk,
			RAW_SIZE_W,
			RAW_SIZE_H);
	if (ret != HD_OK) {
		printf("Run FFC test GFX apply fail=%d\n", ret);
		return -1;
	}

	/* Follow the SDK GFX sample pattern before CPU reads a GFX output VA. */
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_sum_va,
				  blk_size);

	return ffc_compare_gfx_samples(
		p_stream->thermal_ffc_out_va,
		p_stream->thermal_ffc_k_frame_va,
		p_stream->thermal_ffc_sum_va,
		lineoffset,
		RAW_ACTIVE_X,
		RAW_ACTIVE_Y,
		RAW_ACTIVE_W,
		RAW_ACTIVE_H,
		FFC_TEST_SAMPLE_COUNT);
}

static HD_RESULT init_thermal_frame(VIDEO_LIVEVIEW *p_stream)
{
	UINT32 i;
	UINT32 blk_size = VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);
	size_t histogram_bins;
	size_t fieldscale_workspace_values;

	// init TDC output frame
	p_stream->thermal_tdc_out_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_tdc_out_blk == 0) {
		printf("get TDC output block fail\n");
		return HD_ERR_NG;
	}

	p_stream->thermal_tdc_out_pa =
		hd_common_mem_blk2pa(p_stream->thermal_tdc_out_blk);
	if (p_stream->thermal_tdc_out_pa == 0) {
		printf("blk2pa TDC output block fail\n");
		return HD_ERR_NG;
	}

	p_stream->thermal_tdc_out_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE,
		p_stream->thermal_tdc_out_pa, blk_size);
	if (p_stream->thermal_tdc_out_va == 0) {
		printf("mmap TDC output block fail\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_tdc_out_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_tdc_out_va,
				  blk_size);

	//init k frame
	p_stream->thermal_k_frame_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0); // Get block from mem pool
	if (p_stream->thermal_k_frame_blk == 0) {
		printf("get block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_k_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_k_frame_pa = hd_common_mem_blk2pa(p_stream->thermal_k_frame_blk); // Get physical addr
	if (p_stream->thermal_k_frame_pa == 0) {
		printf("blk2pa fail, thermal_k_frame_pa = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_k_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_k_frame_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_k_frame_pa, blk_size); // Get virtual addr
	if (p_stream->thermal_k_frame_va == 0) {
		printf("Error: mmap pa fail !\r\n");
		return HD_ERR_NG;
	}

	/* Initialize NUC unity gain in the selected fixed-point format (Q14). */
	for (i = 0; i < blk_size; i += 4) {
		*(UINT32 *)(p_stream->thermal_k_frame_va + i) =
			(NUC_GAIN_ONE << 16) | NUC_GAIN_ONE;
	}
	hd_common_mem_flush_cache((void *)p_stream->thermal_k_frame_va, blk_size);

	//init b frame
	p_stream->thermal_b_frame_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_b_frame_blk == 0) {
		printf("get b block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_b_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_b_frame_pa = hd_common_mem_blk2pa(p_stream->thermal_b_frame_blk);
	if (p_stream->thermal_b_frame_pa == 0) {
		printf("blk2pa fail, thermal_b_frame_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_b_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_b_frame_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_b_frame_pa, blk_size);
	if (p_stream->thermal_b_frame_va == 0) {
		printf("Error: mmap b pa fail !\r\n");
		return HD_ERR_NG;
	}

	memset((void *)p_stream->thermal_b_frame_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_b_frame_va, blk_size);

	//init nuc out frame
	p_stream->thermal_nuc_out_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0); // Get block from mem pool
	if (p_stream->thermal_nuc_out_blk == 0) {
		printf("get out block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_nuc_out_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_nuc_out_pa = hd_common_mem_blk2pa(p_stream->thermal_nuc_out_blk); // Get physical addr
	if (p_stream->thermal_nuc_out_pa == 0) {
		printf("blk2pa fail, thermal_nuc_out_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_nuc_out_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_nuc_out_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_nuc_out_pa, blk_size); // Get virtual addr
	if (p_stream->thermal_nuc_out_va == 0) {
		printf("Error: mmap out pa fail !\r\n");
		return HD_ERR_NG;
	}
	hd_common_mem_flush_cache((void *)p_stream->thermal_nuc_out_va, blk_size);

	//init ffc k frame to K=1.0 in Q8 format
	p_stream->thermal_ffc_k_frame_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_ffc_k_frame_blk == 0) {
		printf("get ffc k block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_ffc_k_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_k_frame_pa = hd_common_mem_blk2pa(p_stream->thermal_ffc_k_frame_blk);
	if (p_stream->thermal_ffc_k_frame_pa == 0) {
		printf("blk2pa fail, thermal_ffc_k_frame_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_ffc_k_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_k_frame_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_ffc_k_frame_pa, blk_size);
	if (p_stream->thermal_ffc_k_frame_va == 0) {
		printf("Error: mmap ffc k pa fail !\r\n");
		return HD_ERR_NG;
	}

	for (i = 0; i < blk_size; i += 4) {
		*(UINT32 *)(p_stream->thermal_ffc_k_frame_va + i) =
			(FFC_GAIN_ONE << 16) | FFC_GAIN_ONE;
	}
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_k_frame_va, blk_size);

	//init ffc b frame
	p_stream->thermal_ffc_b_frame_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_ffc_b_frame_blk == 0) {
		printf("get ffc b block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_ffc_b_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_b_frame_pa = hd_common_mem_blk2pa(p_stream->thermal_ffc_b_frame_blk);
	if (p_stream->thermal_ffc_b_frame_pa == 0) {
		printf("blk2pa fail, thermal_ffc_b_frame_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_ffc_b_frame_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_b_frame_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_ffc_b_frame_pa, blk_size);
	if (p_stream->thermal_ffc_b_frame_va == 0) {
		printf("Error: mmap ffc b pa fail !\r\n");
		return HD_ERR_NG;
	}

	//init ffc out frame
	p_stream->thermal_ffc_out_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_ffc_out_blk == 0) {
		printf("get ffc out block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_ffc_out_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_out_pa = hd_common_mem_blk2pa(p_stream->thermal_ffc_out_blk);
	if (p_stream->thermal_ffc_out_pa == 0) {
		printf("blk2pa fail, thermal_ffc_out_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_ffc_out_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_out_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_ffc_out_pa, blk_size);
	if (p_stream->thermal_ffc_out_va == 0) {
		printf("Error: mmap ffc out pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_ffc_out_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_out_va, blk_size);

	//init ffc accumulation frame
	p_stream->thermal_ffc_sum_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_ffc_sum_blk == 0) {
		printf("get ffc sum block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_ffc_sum_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_sum_pa = hd_common_mem_blk2pa(p_stream->thermal_ffc_sum_blk);
	if (p_stream->thermal_ffc_sum_pa == 0) {
		printf("blk2pa fail, thermal_ffc_sum_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_ffc_sum_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_sum_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_ffc_sum_pa, blk_size);
	if (p_stream->thermal_ffc_sum_va == 0) {
		printf("Error: mmap ffc sum pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_ffc_sum_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_sum_va, blk_size);

	// init FFC zero frame (read-only GFX source)
	p_stream->thermal_ffc_zero_blk = hd_common_mem_get_block(HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_ffc_zero_blk == 0) {
		printf("get ffc zero block fail (0x%" PRIu64 ")\r\n", (uint64_t)p_stream->thermal_ffc_zero_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_zero_pa = hd_common_mem_blk2pa(p_stream->thermal_ffc_zero_blk);
	if (p_stream->thermal_ffc_zero_pa == 0) {
		printf("blk2pa fail, thermal_ffc_zero_blk = 0x%" PRIu64 "\r\n", (uint64_t)p_stream->thermal_ffc_zero_blk);
		return HD_ERR_NG;
	}

	p_stream->thermal_ffc_zero_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE, p_stream->thermal_ffc_zero_pa, blk_size);
	if (p_stream->thermal_ffc_zero_va == 0) {
		printf("Error: mmap ffc zero pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_ffc_zero_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_ffc_zero_va, blk_size);

	// init shared CPU AGC output frame
	p_stream->thermal_agc_histogram_p1_p99_out_blk =
		hd_common_mem_get_block(
			HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_agc_histogram_p1_p99_out_blk == 0) {
		printf("get shared CPU AGC output block fail\n");
		return HD_ERR_NG;
	}

	p_stream->thermal_agc_histogram_p1_p99_out_pa =
		hd_common_mem_blk2pa(
			p_stream->thermal_agc_histogram_p1_p99_out_blk);
	if (p_stream->thermal_agc_histogram_p1_p99_out_pa == 0) {
		printf("blk2pa shared CPU AGC output block fail\n");
		return HD_ERR_NG;
	}

	p_stream->thermal_agc_histogram_p1_p99_out_va =
		(UINTPTR)hd_common_mem_mmap(
			HD_COMMON_MEM_MEM_TYPE_CACHE,
			p_stream->thermal_agc_histogram_p1_p99_out_pa,
			blk_size);
	if (p_stream->thermal_agc_histogram_p1_p99_out_va == 0) {
		printf("mmap shared CPU AGC output block fail\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_agc_histogram_p1_p99_out_va,
	       0, blk_size);
	hd_common_mem_flush_cache(
		(void *)p_stream->thermal_agc_histogram_p1_p99_out_va,
		blk_size);

	histogram_bins = agc_histogram_p1_p99_workspace_bins();
	p_stream->agc_histogram_p1_p99_workspace =
		(uint32_t *)calloc(histogram_bins, sizeof(uint32_t));
	if (p_stream->agc_histogram_p1_p99_workspace == NULL) {
		printf("allocate Histogram P1/P99 AGC workspace fail, bins=%zu\n",
		       histogram_bins);
		return HD_ERR_NG;
	}
	agc_histogram_p1_p99_state_reset(
		&p_stream->agc_histogram_p1_p99_state);
	fieldscale_workspace_values =
		agc_fieldscale_horizontal_workspace_values(RAW_SIZE_W);
	p_stream->agc_fieldscale_horizontal_workspace = (uint32_t *)calloc(
		fieldscale_workspace_values, sizeof(uint32_t));
	if (p_stream->agc_fieldscale_horizontal_workspace == NULL) {
		printf("allocate Fieldscale horizontal workspace fail, values=%zu\n",
		       fieldscale_workspace_values);
		return HD_ERR_NG;
	}
	agc_fieldscale_state_reset(&p_stream->agc_fieldscale_state);

	// init ISP RAW14-to-Y8 step-test input frame
	p_stream->thermal_isp_step_test_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, blk_size, DDR_ID0);
	if (p_stream->thermal_isp_step_test_blk == 0) {
		printf("get ISP RAW-to-Y Step Test input block fail\n");
		return HD_ERR_NG;
	}
	p_stream->thermal_isp_step_test_pa = hd_common_mem_blk2pa(
		p_stream->thermal_isp_step_test_blk);
	if (p_stream->thermal_isp_step_test_pa == 0) {
		printf("blk2pa ISP RAW-to-Y Step Test input block fail\n");
		return HD_ERR_NG;
	}
	p_stream->thermal_isp_step_test_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE,
		p_stream->thermal_isp_step_test_pa, blk_size);
	if (p_stream->thermal_isp_step_test_va == 0) {
		printf("mmap ISP RAW-to-Y Step Test input block fail\n");
		return HD_ERR_NG;
	}
	memset((void *)p_stream->thermal_isp_step_test_va, 0, blk_size);
	hd_common_mem_flush_cache((void *)p_stream->thermal_isp_step_test_va,
		blk_size);

	return HD_OK;
}
static HD_RESULT release_thermal_frame_buffer(UINTPTR *p_va,
	HD_COMMON_MEM_VB_BLK *p_blk, UINT32 blk_size, const char *name)
{
	HD_RESULT ret = HD_OK;

	if (*p_va != 0) {
		ret = hd_common_mem_munmap((void *)*p_va, blk_size);
		if (ret != HD_OK) {
			printf("munmap %s error=%d\n", name, ret);
		}
		*p_va = 0;
	}

	if (*p_blk != 0) {
		HD_RESULT release_ret = hd_common_mem_release_block(*p_blk);
		if (release_ret != HD_OK) {
			printf("release %s block error=%d\n", name, release_ret);
			if (ret == HD_OK) {
				ret = release_ret;
			}
		}
		*p_blk = 0;
	}

	return ret;
}

static HD_RESULT exit_thermal_frame(VIDEO_LIVEVIEW *p_stream)
{
	HD_RESULT ret = HD_OK;
	HD_RESULT release_ret;
	UINT32 blk_size = VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);

#define RELEASE_THERMAL_BUFFER(va, blk, name) \
	do { \
		release_ret = release_thermal_frame_buffer(&(va), &(blk), blk_size, (name)); \
		if (ret == HD_OK && release_ret != HD_OK) { \
			ret = release_ret; \
		} \
	} while (0)

	RELEASE_THERMAL_BUFFER(p_stream->thermal_k_frame_va,
		p_stream->thermal_k_frame_blk, "NUC K");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_tdc_out_va,
		p_stream->thermal_tdc_out_blk, "TDC output");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_b_frame_va,
		p_stream->thermal_b_frame_blk, "NUC B");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_nuc_out_va,
		p_stream->thermal_nuc_out_blk, "NUC output");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_ffc_k_frame_va,
		p_stream->thermal_ffc_k_frame_blk, "FFC K");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_ffc_b_frame_va,
		p_stream->thermal_ffc_b_frame_blk, "FFC B");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_ffc_out_va,
		p_stream->thermal_ffc_out_blk, "FFC output");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_ffc_sum_va,
		p_stream->thermal_ffc_sum_blk, "FFC sum");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_ffc_zero_va,
		p_stream->thermal_ffc_zero_blk, "FFC zero");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_agc_histogram_p1_p99_out_va,
		p_stream->thermal_agc_histogram_p1_p99_out_blk,
		"shared CPU AGC output");
	RELEASE_THERMAL_BUFFER(p_stream->thermal_isp_step_test_va,
		p_stream->thermal_isp_step_test_blk,
		"ISP RAW-to-Y Step Test input");

#undef RELEASE_THERMAL_BUFFER

	p_stream->thermal_k_frame_pa = 0;
	p_stream->thermal_tdc_out_pa = 0;
	p_stream->thermal_b_frame_pa = 0;
	p_stream->thermal_nuc_out_pa = 0;
	p_stream->thermal_ffc_k_frame_pa = 0;
	p_stream->thermal_ffc_b_frame_pa = 0;
	p_stream->thermal_ffc_out_pa = 0;
	p_stream->thermal_ffc_sum_pa = 0;
	p_stream->thermal_ffc_zero_pa = 0;
	p_stream->thermal_agc_histogram_p1_p99_out_pa = 0;
	p_stream->thermal_isp_step_test_pa = 0;
	free(p_stream->agc_histogram_p1_p99_workspace);
	p_stream->agc_histogram_p1_p99_workspace = NULL;
	free(p_stream->agc_fieldscale_horizontal_workspace);
	p_stream->agc_fieldscale_horizontal_workspace = NULL;
	p_stream->agc_histogram_p1_p99_frame_count = 0;
	p_stream->agc_histogram_p1_p99_error_count = 0;
	agc_histogram_p1_p99_state_reset(
		&p_stream->agc_histogram_p1_p99_state);
	agc_fieldscale_state_reset(&p_stream->agc_fieldscale_state);
	p_stream->agc_fieldscale_frame_count = 0;
	p_stream->agc_fieldscale_error_count = 0;
	p_stream->agc_min_max_tone_curve_frame_count = 0;
	p_stream->agc_min_max_tone_curve_error_count = 0;

	return ret;
}

static void skip_vcap_frame(VIDEO_LIVEVIEW *p_stream0, UINT32 skip_cnt)
{
	UINT32 i;
	HD_VIDEO_FRAME vcap_video_frame = {0};
	HD_RESULT ret = HD_OK;

	for (i = 0; i < skip_cnt && p_stream0->cap_exit == 0;) {
		ret = hd_videocap_pull_out_buf(p_stream0->cap_path, &vcap_video_frame, 100);
		if (ret != HD_OK) {
			if (ret != HD_ERR_UNDERRUN) {
				printf("skip_vcap_frame vcap pull error=%d !!\r\n\r\n", ret);
			}
			continue;
		}
		ret = hd_videocap_release_out_buf(p_stream0->cap_path, &vcap_video_frame);
		if (ret != HD_OK) {
			printf("skip_vcap_frame cap_release error=%d !!\r\n\r\n", ret);
		}
		i++;
	}
}

static HD_RESULT init_module(void)
{
	HD_RESULT ret;
	if ((ret = hd_videocap_init()) != HD_OK)
		return ret;
	if ((ret = hd_videoproc_init()) != HD_OK) {
		hd_videocap_uninit();
		return ret;
	}
	if ((ret = video_output_module_init()) != HD_OK) {
		hd_videoproc_uninit();
		hd_videocap_uninit();
		return ret;
	}
	if ((ret = hd_gfx_init()) != HD_OK) {
		video_output_module_uninit();
		hd_videoproc_uninit();
		hd_videocap_uninit();
		return ret;
	}
	return HD_OK;
}

static HD_RESULT open_module(VIDEO_LIVEVIEW *p_stream, HD_DIM* p_proc_max_dim)
{
	HD_RESULT ret;
	// set videocap config
	ret = set_cap_cfg(&p_stream->cap_ctrl);
	if (ret != HD_OK) {
		printf("set cap-cfg fail=%d\n", ret);
		return HD_ERR_NG;
	}
	// set videoproc config
	ret = set_proc_cfg(&p_stream->proc_ctrl, p_proc_max_dim, HD_VIDEOPROC_0_CTRL);
	if (ret != HD_OK) {
		printf("set proc-cfg fail=%d\n", ret);
		return HD_ERR_NG;
	}
	if ((ret = hd_videocap_open(HD_VIDEOCAP_IN(g_thermal_pipeline_id, 0), HD_VIDEOCAP_OUT(g_thermal_pipeline_id, 0), &p_stream->cap_path)) != HD_OK)
		return ret;
	if ((ret = hd_videoproc_open(HD_VIDEOPROC_0_IN_0, HD_VIDEOPROC_0_OUT_0, &p_stream->proc_path)) != HD_OK)
		return ret;
	if (videoproc_isp_is_3dnr_enabled()) {
		if ((ret = hd_videoproc_open(HD_VIDEOPROC_0_IN_0,
			HD_VIDEOPROC_0_OUT_4,
			&p_stream->proc_path_3dnr)) != HD_OK) {
			return ret;
		}
	}
	ret = video_output_open(&p_stream->video_output, g_output_mode,
		VIDEO_OUTPUT_WIDTH, VIDEO_OUTPUT_HEIGHT, g_fps,
		RTSP_OUTPUT_BITRATE_BPS, g_thermal_pipeline_id);
	if (ret != HD_OK)
		return ret;

	return HD_OK;
}

static HD_RESULT close_module(VIDEO_LIVEVIEW *p_stream)
{
	HD_RESULT ret = HD_OK;
	HD_RESULT close_ret;

#define CLOSE_PATH(path, close_fn, name) \
	do { \
		if ((path) != 0) { \
			close_ret = close_fn(path); \
			if (close_ret != HD_OK) { \
				printf("close %s fail=%d\n", (name), close_ret); \
				if (ret == HD_OK) ret = close_ret; \
			} \
			(path) = 0; \
		} \
	} while (0)

	close_ret = video_output_close(&p_stream->video_output);
	if (close_ret != HD_OK && ret == HD_OK) {
		ret = close_ret;
	}
	CLOSE_PATH(p_stream->cap_path, hd_videocap_close, "videocap");
	CLOSE_PATH(p_stream->proc_path, hd_videoproc_close, "videoproc");
	CLOSE_PATH(p_stream->proc_path_3dnr, hd_videoproc_close, "videoproc 3DNR");
#undef CLOSE_PATH

	return ret;
}

static HD_RESULT exit_module(void)
{
	HD_RESULT ret = HD_OK;
	HD_RESULT uninit_ret;

#define UNINIT_MODULE(uninit_fn, name) \
	do { \
		uninit_ret = uninit_fn(); \
		if (uninit_ret != HD_OK) { \
			printf("uninit %s fail=%d\n", (name), uninit_ret); \
			if (ret == HD_OK) ret = uninit_ret; \
		} \
	} while (0)

	UNINIT_MODULE(hd_gfx_uninit, "gfx");
	UNINIT_MODULE(video_output_module_uninit, "videoenc");
	UNINIT_MODULE(hd_videoproc_uninit, "videoproc");
	UNINIT_MODULE(hd_videocap_uninit, "videocap");

#undef UNINIT_MODULE

	return ret;
}

/* Runtime RAW pipeline helpers. */
static void liveview_update_temperature_tables_if_needed(
	UINT64 *nuc_next_update_ms,
	UINT64 *tdc_next_update_ms)
{
	UINT64 now_ms = liveview_get_time_ms();

	/*
	 * Manual BPR/NUC calibration owns the correction tables while it is
	 * active. Keep the periodic NUC/TDC loaders out of the same buffers and
	 * postpone their next check until one full interval after calibration.
	 */
	pthread_mutex_lock(&g_manual_calibration_lock);
	if (g_manual_calibration_active) {
		*nuc_next_update_ms =
			now_ms +
			(UINT64)nuc_get_sensor_temp_interval_sec() * 1000U;
		*tdc_next_update_ms =
			now_ms + (UINT64)tdc_get_interval_sec() * 1000U;
		pthread_mutex_unlock(&g_manual_calibration_lock);
		return;
	}

	if (nuc_is_enabled() &&
	    !nuc_is_blackbody_dynamic_enabled() &&
	    now_ms >= *nuc_next_update_ms) {
		nuc_update_sensor_temp_table_periodic();
		*nuc_next_update_ms =
			now_ms +
			(UINT64)nuc_get_sensor_temp_interval_sec() * 1000U;
	}

	if (tdc_is_enabled() && now_ms >= *tdc_next_update_ms) {
		if (tdc_update_correction_table() != HD_OK) {
			printf("TDC update fail\n");
		}
		*tdc_next_update_ms =
			now_ms + (UINT64)tdc_get_interval_sec() * 1000U;
	}
	pthread_mutex_unlock(&g_manual_calibration_lock);
}

static int liveview_run_ffc_calibration(
	VIDEO_LIVEVIEW *p_stream0,
	UINT64 *nuc_next_update_ms,
	UINT64 *tdc_next_update_ms,
	int test_mode,
	int startup_ffc)
{
	HD_VIDEO_FRAME vcap_video_frame = {0};
	HD_VIDEO_FRAME tdc_video_frame = {0};
	HD_VIDEO_FRAME nuc_video_frame = {0};
	UINT32 raw_buffer_size =
		VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);
	UINT32 lineoffset = 0;
	UINT32 frame;
	int final_release_failed = 0;

	memset((void *)p_stream0->thermal_ffc_out_va, 0, raw_buffer_size);
	hd_common_mem_flush_cache((void *)p_stream0->thermal_ffc_out_va,
				  raw_buffer_size);
	printf("FFC average %u frame start\n", FFC_AVERAGE_FRAME_COUNT);

	for (frame = 0; frame < FFC_AVERAGE_FRAME_COUNT; frame++) {
		HD_VIDEO_FRAME *calibration_frame = &vcap_video_frame;
		HD_RESULT process_ret = HD_OK;
		HD_RESULT release_ret;
		UINTPTR sum_pa;
		UINTPTR out_pa;
		UINT32 shift = 0;

		if (p_stream0->cap_exit) {
			printf("FFC calibration stopped\n");
			return -1;
		}

		/* The old outer loop refreshed these tables between FFC frames. */
		if (frame > 0) {
			liveview_update_temperature_tables_if_needed(
				nuc_next_update_ms, tdc_next_update_ms);
			if (!test_mode && !startup_ffc &&
			    (!ffc_is_enabled() || ffc_get_shutter_interval_sec() == 0)) {
				printf("Shutter calibration disabled during FFC\n");
				return -1;
			}
		}

		process_ret = hd_videocap_pull_out_buf(
			p_stream0->cap_path, &vcap_video_frame, 100);
		if (process_ret != HD_OK) {
			if (process_ret != HD_ERR_UNDERRUN) {
				printf("FFC cap_pull error=%d !!\r\n\r\n",
				       process_ret);
			}
			return -1;
		}

		lineoffset = vcap_video_frame.loff[0];

		if (tdc_is_enabled()) {
			process_ret = tdc_apply(calibration_frame,
						&tdc_video_frame,
						p_stream0->thermal_tdc_out_pa,
						p_stream0->thermal_tdc_out_blk,
						RAW_SIZE_W,
						RAW_SIZE_H);
			if (process_ret != HD_OK) {
				printf("FFC calibration TDC error=%d\n",
				       process_ret);
			} else {
				calibration_frame = &tdc_video_frame;
			}
		}

		if (process_ret == HD_OK && nuc_is_enabled()) {
			if (nuc_prepare_blackbody_dynamic_table(
				    calibration_frame) != 0) {
				printf("FFC calibration dynamic NUC table build fail\n");
				process_ret = HD_ERR_NG;
			} else {
				process_ret = nuc_apply(calibration_frame,
						&nuc_video_frame,
						p_stream0->thermal_k_frame_pa,
						p_stream0->thermal_b_frame_pa,
						p_stream0->thermal_nuc_out_pa,
						p_stream0->thermal_nuc_out_blk,
						RAW_SIZE_W,
						RAW_SIZE_H);
			}
			if (process_ret != HD_OK) {
				printf("FFC calibration NUC error=%d\n",
				       process_ret);
			} else {
				calibration_frame = &nuc_video_frame;
			}
		}

		if ((frame & 1U) == 0) {
			sum_pa = p_stream0->thermal_ffc_out_pa;
			out_pa = p_stream0->thermal_ffc_sum_pa;
		} else {
			sum_pa = p_stream0->thermal_ffc_out_pa;
			out_pa = p_stream0->thermal_ffc_sum_pa;
		}
		if (frame == FFC_AVERAGE_FRAME_COUNT - 1U) {
			shift = FFC_AVERAGE_SHIFT;
		}

		if (process_ret == HD_OK) {
			process_ret = ffc_accumulate_frame(
				calibration_frame->phy_addr[0],
				sum_pa, out_pa, p_stream0->thermal_ffc_zero_pa,
				RAW_SIZE_W, RAW_SIZE_H, shift);
			if (process_ret == HD_OK) {
				printf("FFC average frame %u/%u, shift=%u\n",
				       frame + 1U, FFC_AVERAGE_FRAME_COUNT,
				       shift);
			} else {
				printf("FFC accumulate error=%d\n", process_ret);
			}
		}

		release_ret = hd_videocap_release_out_buf(
			p_stream0->cap_path, &vcap_video_frame);
		if (release_ret != HD_OK) {
			printf("FFC cap_release error=%d !!\r\n\r\n",
			       release_ret);
		}

		if (process_ret != HD_OK) {
			return -1;
		}
		if (release_ret != HD_OK) {
			if (frame != FFC_AVERAGE_FRAME_COUNT - 1U) {
				return -1;
			}
			final_release_failed = 1;
		}
		if (frame != FFC_AVERAGE_FRAME_COUNT - 1U) {
			usleep(100);
		}
	}

	if (liveview_update_ffc_table_from_average(p_stream0, lineoffset) != 0) {
		printf("FFC update table fail\n");
		return -1;
	} else if (test_mode &&
		   liveview_run_ffc_gfx_test(p_stream0, lineoffset) != 0) {
		return -1;
	}

	return final_release_failed ? -1 : 0;
}

static int liveview_run_shutter_calibration_cycle(
	VIDEO_LIVEVIEW *p_stream0,
	UINT64 *nuc_next_update_ms,
	UINT64 *tdc_next_update_ms,
	int test_mode,
	int startup_ffc)
{
	int calibration_ok = 1;
	int shutter_result = 0;

	pthread_mutex_lock(&g_shutter_calibration_lock);
	/* Recheck after taking the lock: the interval may have changed since
	 * the worker checked its deadline. Startup/manual FFC bypass this timer. */
	if (!test_mode && !startup_ffc &&
	    (!ffc_is_enabled() || ffc_get_shutter_interval_sec() == 0)) {
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		return SHUTTER_CALIBRATION_PAUSED;
	}
	pthread_mutex_lock(&g_manual_calibration_lock);
	if (g_manual_calibration_active) {
		pthread_mutex_unlock(&g_manual_calibration_lock);
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		return SHUTTER_CALIBRATION_PAUSED;
	}
	pthread_mutex_unlock(&g_manual_calibration_lock);

	if (gpio_shutter_close() != 0) {
		printf("Shutter calibration close shutter fail\n");
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		return -1;
	}

	liveview_sleep_ms(g_shutter_close_wait_ms);

	if (test_mode || startup_ffc || ffc_is_enabled()) {
		if (liveview_run_ffc_calibration(p_stream0,
						 nuc_next_update_ms,
						 tdc_next_update_ms,
						 test_mode, startup_ffc) != 0) {
			calibration_ok = 0;
		}
	}


	if (gpio_shutter_open() != 0) {
		printf("Shutter calibration open shutter fail\n");
		shutter_result = -1;
	}

	pthread_mutex_unlock(&g_shutter_calibration_lock);
	if ((test_mode || startup_ffc) && !calibration_ok) {
		return -1;
	}
	return shutter_result;
}

static int liveview_apply_agc_histogram_p1_p99(
	VIDEO_LIVEVIEW *p_stream,
	const HD_VIDEO_FRAME *input_frame,
	HD_VIDEO_FRAME *output_frame,
	uint16_t *p1_result,
	uint16_t *p99_result)
{
	UINTPTR block_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR input_va;
	UINT64 data_offset;
	UINT64 map_size_64;
	UINT32 map_size;
	UINT32 input_stride;
	UINT32 output_stride;
	UINT32 plane_height;
	UINT32 output_size;
	size_t histogram_bins;
	int agc_status;
	HD_RESULT unmap_ret;

	if (p_stream == NULL || input_frame == NULL || output_frame == NULL ||
	    input_frame->phy_addr[0] == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_pa == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_va == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_blk == 0 ||
	    p_stream->agc_histogram_p1_p99_workspace == NULL) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	if (input_frame->dim.w != RAW_SIZE_W ||
	    input_frame->dim.h != RAW_SIZE_H) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	output_stride = RAW_SIZE_W * sizeof(uint16_t);
	output_size = output_stride * RAW_SIZE_H;
	input_stride = input_frame->loff[0];
	if (input_stride < output_stride) {
		input_stride *= sizeof(uint16_t);
	}
	plane_height = input_frame->ph[0] != 0 ?
		input_frame->ph[0] : input_frame->dim.h;
	if (input_stride < output_stride || plane_height < RAW_SIZE_H) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	block_pa = input_frame->blk != 0 ?
		hd_common_mem_blk2pa(input_frame->blk) : 0;
	if (block_pa != 0) {
		if (input_frame->phy_addr[0] < block_pa) {
			return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
		}
		map_pa = block_pa;
		data_offset = (UINT64)(input_frame->phy_addr[0] - block_pa);
	} else {
		map_pa = input_frame->phy_addr[0];
		data_offset = 0;
	}

	map_size_64 = data_offset + (UINT64)input_stride * plane_height;
	if (map_size_64 == 0 || map_size_64 > UINT_MAX) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}
	map_size = (UINT32)map_size_64;
	map_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, map_pa, map_size);
	if (map_va == 0) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	/* Synchronize the hardware/VCAP writer before the CPU reads this frame. */
	hd_common_mem_flush_cache((void *)map_va, map_size);
	input_va = map_va + (UINTPTR)data_offset;
	histogram_bins = agc_histogram_p1_p99_workspace_bins();
	agc_status = agc_histogram_p1_p99_apply_u14(
		(const uint16_t *)input_va,
		input_stride,
		(uint16_t *)p_stream->thermal_agc_histogram_p1_p99_out_va,
		output_stride,
		RAW_SIZE_W,
		RAW_SIZE_H,
		p_stream->agc_histogram_p1_p99_workspace,
		histogram_bins,
		&p_stream->agc_histogram_p1_p99_state,
		p1_result,
		p99_result);

	if (agc_status >= AGC_HISTOGRAM_P1_P99_STATUS_OK) {
		/* The CPU wrote the output; publish it before VideoProc reads it. */
		hd_common_mem_flush_cache(
			(void *)p_stream->thermal_agc_histogram_p1_p99_out_va,
			output_size);
		memcpy(output_frame, input_frame, sizeof(*output_frame));
		output_frame->phy_addr[0] =
			p_stream->thermal_agc_histogram_p1_p99_out_pa;
		output_frame->blk =
			p_stream->thermal_agc_histogram_p1_p99_out_blk;
		output_frame->loff[0] = output_stride;
		output_frame->ph[0] = RAW_SIZE_H;
	}

	unmap_ret = hd_common_mem_munmap((void *)map_va, map_size);
	if (unmap_ret != HD_OK) {
		return AGC_HISTOGRAM_P1_P99_STATUS_INVALID_ARGUMENT;
	}

	return agc_status;
}

static int liveview_apply_agc_fieldscale(
	VIDEO_LIVEVIEW *p_stream,
	const HD_VIDEO_FRAME *input_frame,
	HD_VIDEO_FRAME *output_frame)
{
	UINTPTR block_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR input_va;
	UINT64 data_offset;
	UINT64 map_size_64;
	UINT32 map_size;
	UINT32 input_stride;
	UINT32 output_stride;
	UINT32 plane_height;
	UINT32 output_size;
	int agc_status;
	HD_RESULT unmap_ret;
	AGC_FIELDSCALE_LATENCY_SAMPLE latency = {0};
	UINT64 total_start_us = 0;
	UINT64 map_cache_start_us = 0;
	UINT64 map_cache_done_us = 0;
	UINT64 core_done_us = 0;
	UINT64 output_flush_start_us = 0;
	UINT64 output_flush_done_us = 0;
	UINT64 unmap_start_us = 0;
	int measure_latency;

	measure_latency = agc_fieldscale_latency_is_active();
	if (measure_latency) {
		total_start_us = liveview_get_time_us();
	}

	if (p_stream == NULL || input_frame == NULL || output_frame == NULL ||
	    input_frame->phy_addr[0] == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_pa == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_va == 0 ||
	    p_stream->thermal_agc_histogram_p1_p99_out_blk == 0 ||
	    p_stream->agc_fieldscale_horizontal_workspace == NULL ||
	    input_frame->dim.w != RAW_SIZE_W ||
	    input_frame->dim.h != RAW_SIZE_H) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}

	output_stride = RAW_SIZE_W * sizeof(uint16_t);
	output_size = output_stride * RAW_SIZE_H;
	input_stride = input_frame->loff[0];
	if (input_stride < output_stride) {
		input_stride *= sizeof(uint16_t);
	}
	plane_height = input_frame->ph[0] != 0U ?
		input_frame->ph[0] : input_frame->dim.h;
	if (input_stride < output_stride || plane_height < RAW_SIZE_H) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}

	block_pa = input_frame->blk != 0 ?
		hd_common_mem_blk2pa(input_frame->blk) : 0;
	if (block_pa != 0) {
		if (input_frame->phy_addr[0] < block_pa) {
			return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
		}
		map_pa = block_pa;
		data_offset = (UINT64)(input_frame->phy_addr[0] - block_pa);
	} else {
		map_pa = input_frame->phy_addr[0];
		data_offset = 0;
	}

	map_size_64 = data_offset + (UINT64)input_stride * plane_height;
	if (map_size_64 == 0U || map_size_64 > UINT_MAX) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}
	map_size = (UINT32)map_size_64;
	if (measure_latency) {
		map_cache_start_us = liveview_get_time_us();
	}
	map_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, map_pa, map_size);
	if (map_va == 0) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}

	/* Synchronize the hardware/previous correction writer before CPU reads. */
	hd_common_mem_flush_cache((void *)map_va, map_size);
	if (measure_latency) {
		map_cache_done_us = liveview_get_time_us();
		latency.map_cache_us = map_cache_done_us - map_cache_start_us;
	}
	input_va = map_va + (UINTPTR)data_offset;
	agc_status = agc_fieldscale_apply_u14(
		(const uint16_t *)input_va,
		input_stride,
		(uint16_t *)p_stream->thermal_agc_histogram_p1_p99_out_va,
		output_stride,
		RAW_SIZE_W,
		RAW_SIZE_H,
		&p_stream->agc_fieldscale_state,
		p_stream->agc_fieldscale_horizontal_workspace,
		agc_fieldscale_horizontal_workspace_values(RAW_SIZE_W));
	if (measure_latency) {
		core_done_us = liveview_get_time_us();
		latency.core_us = core_done_us - map_cache_done_us;
	}

	if (agc_status == AGC_FIELDSCALE_STATUS_OK) {
		if (measure_latency) {
			output_flush_start_us = liveview_get_time_us();
		}
		hd_common_mem_flush_cache(
			(void *)p_stream->thermal_agc_histogram_p1_p99_out_va,
			output_size);
		if (measure_latency) {
			output_flush_done_us = liveview_get_time_us();
			latency.output_flush_us = output_flush_done_us -
				output_flush_start_us;
		}
		memcpy(output_frame, input_frame, sizeof(*output_frame));
		output_frame->phy_addr[0] =
			p_stream->thermal_agc_histogram_p1_p99_out_pa;
		output_frame->blk =
			p_stream->thermal_agc_histogram_p1_p99_out_blk;
		output_frame->loff[0] = output_stride;
		output_frame->ph[0] = RAW_SIZE_H;
	}
	if (measure_latency) {
		unmap_start_us = liveview_get_time_us();
	}

	unmap_ret = hd_common_mem_munmap((void *)map_va, map_size);
	if (measure_latency) {
		latency.completed_at_us = liveview_get_time_us();
		latency.unmap_us = latency.completed_at_us - unmap_start_us;
		latency.total_us = latency.completed_at_us - total_start_us;
	}
	if (unmap_ret != HD_OK) {
		return AGC_FIELDSCALE_STATUS_INVALID_ARGUMENT;
	}
	if (measure_latency && agc_status == AGC_FIELDSCALE_STATUS_OK) {
		agc_fieldscale_latency_record(&latency);
	}
	return agc_status;
}

static int liveview_apply_agc_min_max_tone_curve(
	const HD_VIDEO_FRAME *input_frame,
	AGC_MIN_MAX_TONE_CURVE_LUT *lut)
{
	IQT_TONE_PARAM tone = {0};
	UINTPTR block_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR input_va;
	UINT64 data_offset;
	UINT64 map_size_64;
	UINT32 map_size;
	UINT32 input_stride;
	UINT32 minimum_stride;
	UINT32 plane_height;
	UINT32 i;
	int agc_status;
	HD_RESULT iq_ret;
	HD_RESULT unmap_ret;

	if (input_frame == NULL || lut == NULL ||
	    input_frame->phy_addr[0] == 0) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}
	if (input_frame->dim.w != RAW_SIZE_W ||
	    input_frame->dim.h != RAW_SIZE_H) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}

	minimum_stride = RAW_SIZE_W * sizeof(uint16_t);
	input_stride = input_frame->loff[0];
	if (input_stride < minimum_stride) {
		input_stride *= sizeof(uint16_t);
	}
	plane_height = input_frame->ph[0] != 0 ?
		input_frame->ph[0] : input_frame->dim.h;
	if (input_stride < minimum_stride || plane_height < RAW_SIZE_H) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}

	block_pa = input_frame->blk != 0 ?
		hd_common_mem_blk2pa(input_frame->blk) : 0;
	if (block_pa != 0) {
		if (input_frame->phy_addr[0] < block_pa) {
			return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
		}
		map_pa = block_pa;
		data_offset = (UINT64)(input_frame->phy_addr[0] - block_pa);
	} else {
		map_pa = input_frame->phy_addr[0];
		data_offset = 0;
	}

	map_size_64 = data_offset + (UINT64)input_stride * plane_height;
	if (map_size_64 == 0 || map_size_64 > UINT_MAX) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}
	map_size = (UINT32)map_size_64;
	map_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, map_pa, map_size);
	if (map_va == 0) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}

	/* Synchronize the hardware/VCAP writer before the CPU scans this frame. */
	hd_common_mem_flush_cache((void *)map_va, map_size);
	input_va = map_va + (UINTPTR)data_offset;
	agc_status = agc_min_max_tone_curve_build_u14(
		(const uint16_t *)input_va,
		input_stride,
		RAW_SIZE_W,
		RAW_SIZE_H,
		lut);

	if (agc_status >= AGC_MIN_MAX_TONE_CURVE_STATUS_OK) {
		tone.id = g_thermal_pipeline_id;
		iq_ret = vendor_isp_get_iq(IQT_ITEM_TONE_PARAM, &tone);
		if (iq_ret != HD_OK) {
			agc_status = AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
		} else {
			tone.tone.enable = 1;
			tone.tone.mode = 1;
			tone.tone.auto_sel = 2;
			for (i = 0;
			     i < AGC_MIN_MAX_TONE_CURVE_LEFT_LUT_COUNT;
			     i++) {
				tone.tone.manual_lut_left[i] = lut->left[i];
			}
			for (i = 0;
			     i < AGC_MIN_MAX_TONE_CURVE_RIGHT_LUT_COUNT;
			     i++) {
				tone.tone.manual_lut_right[i] = lut->right[i];
			}

			iq_ret = vendor_isp_set_iq(IQT_ITEM_TONE_PARAM, &tone);
			if (iq_ret != HD_OK) {
				agc_status =
					AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
			}
		}
	}

	unmap_ret = hd_common_mem_munmap((void *)map_va, map_size);
	if (unmap_ret != HD_OK) {
		return AGC_MIN_MAX_TONE_CURVE_STATUS_INVALID_ARGUMENT;
	}

	return agc_status;
}

/* Only the RAW worker changes active AGC and ISP tone state. */
static void liveview_apply_agc_request(VIDEO_LIVEVIEW *stream)
{
    int requested = agc_mode_take_request();
    AGC_MODE old_mode = agc_mode_get();
    IQT_TONE_PARAM target;
    if (requested < 0 || requested == (int)old_mode) return;
    if (!g_agc_baseline_valid) {
        printf("AGC switch unavailable: baseline ISP tone was not captured\n");
        return;
    }
    target = g_agc_baseline_tone;
    if (requested == AGC_MODE_HISTOGRAM_P1_P99_CPU ||
        requested == AGC_MODE_FIELDSCALE_CPU) target.tone.enable = 0;
    if (vendor_isp_set_iq(IQT_ITEM_TONE_PARAM, &target) != HD_OK) {
        printf("AGC switch failed: ISP tone update failed; runtime unchanged\n");
        return;
    }
    agc_histogram_p1_p99_state_reset(
        &stream->agc_histogram_p1_p99_state);
    agc_fieldscale_state_reset(&stream->agc_fieldscale_state);
    agc_mode_set((AGC_MODE)requested);
    printf("AGC applied: %s\n", agc_mode_name((AGC_MODE)requested));
}

static void liveview_process_live_frame(VIDEO_LIVEVIEW *p_stream0)
{
	HD_VIDEO_FRAME vcap_video_frame = {0};
	HD_VIDEO_FRAME tdc_video_frame = {0};
	HD_VIDEO_FRAME nuc_video_frame = {0};
	HD_VIDEO_FRAME ffc_video_frame = {0};
	HD_VIDEO_FRAME agc_histogram_p1_p99_video_frame = {0};
	HD_VIDEO_FRAME isp_step_test_video_frame = {0};
	HD_VIDEO_FRAME *current_frame = &vcap_video_frame;
	HD_VIDEO_FRAME *snapshot_final_frame = NULL;
	HD_VIDEO_FRAME *snapshot_agc_output_frame = NULL;
	int current_frame_is_vcap = 1;
	int snapshot_requested = 0;
	int snapshot_final_is_input = 0;
	int snapshot_has_agc_output = 0;
	int timed_snapshot_job = 0;
	struct timespec capture_wall_time;
	int64_t capture_unix_ms = -1;
	int raw_copy_handled = 0;
	int correction_lock_held = 0;
	int nuc_applied = 0;
	int isp_step_test_requested = 0;
	UINT32 isp_step_test_run_id = 0;
	UINT32 isp_step_test_min_raw_dn = 0U;
	UINT32 isp_step_test_max_raw_dn = ISP_RAW_TO_Y_STEP_RAW14_MAX;
	int agc_status;
	uint16_t agc_p1 = 0;
	uint16_t agc_p99 = 0;
	AGC_MIN_MAX_TONE_CURVE_LUT agc_tone_curve_lut = {0};
	UINT64 agc_start_ms;
	UINT64 agc_end_ms;
	UINT64 agc_elapsed_ms;
	HD_RESULT ret;
	HD_RESULT push_ret;

	ret = hd_videocap_pull_out_buf(
		p_stream0->cap_path, &vcap_video_frame, 100);
	if (ret != HD_OK) {
		if (ret != HD_ERR_UNDERRUN) {
			printf("cap_pull error=%d !!\r\n\r\n", ret);
		}
		return;
	}
	if (clock_gettime(CLOCK_REALTIME, &capture_wall_time) == 0)
		capture_unix_ms = (int64_t)capture_wall_time.tv_sec * 1000 +
			capture_wall_time.tv_nsec / 1000000;
	liveview_apply_agc_request(p_stream0);

	if (g_raw_copy_pending) {
		g_raw_copy_result =
			copy_videoframe_raw(&vcap_video_frame,
					    g_raw_copy_dst_va,
					    g_raw_copy_size);
		g_raw_copy_pending = 0;
		g_raw_copy_done = 1;
		raw_copy_handled = 1;
	}

	/*
	 * A calibration callback first waits for any in-flight correction here,
	 * then marks manual calibration active. While active, keep pulling VCAP
	 * frames for calibration RAW requests but bypass TDC/NUC/FFC so none of
	 * them reads a table buffer that calibration may be replacing.
	 */
	pthread_mutex_lock(&g_manual_calibration_lock);
	correction_lock_held = 1;
	if (!g_manual_calibration_active &&
	    liveview_take_isp_step_test_request(&isp_step_test_run_id,
					    &isp_step_test_min_raw_dn,
					    &isp_step_test_max_raw_dn)) {
		UINT32 test_stride = RAW_SIZE_W * sizeof(uint16_t);
		UINT32 test_size = test_stride * RAW_SIZE_H;

		if (isp_raw_to_y_step_diagnostic_generate(
				(uint16_t *)p_stream0->thermal_isp_step_test_va,
				test_stride, RAW_SIZE_W, RAW_SIZE_H,
				isp_step_test_min_raw_dn,
				isp_step_test_max_raw_dn) != 0) {
			printf("ISP RAW-to-Y Step Test: generate RAW14 step frame fail\n");
			pthread_mutex_unlock(&g_manual_calibration_lock);
			correction_lock_held = 0;
			liveview_finish_isp_step_test(-1);
			goto release_vcap_frame;
		}
		hd_common_mem_flush_cache(
			(void *)p_stream0->thermal_isp_step_test_va, test_size);
		memcpy(&isp_step_test_video_frame, &vcap_video_frame,
		       sizeof(isp_step_test_video_frame));
		isp_step_test_video_frame.phy_addr[0] =
			p_stream0->thermal_isp_step_test_pa;
		isp_step_test_video_frame.blk =
			p_stream0->thermal_isp_step_test_blk;
		isp_step_test_video_frame.loff[0] = test_stride;
		isp_step_test_video_frame.ph[0] = RAW_SIZE_H;
		current_frame = &isp_step_test_video_frame;
		current_frame_is_vcap = 0;
		isp_step_test_requested = 1;
		printf("ISP RAW-to-Y Step Test %04u: generated %u bands "
		       "(%u..%u in RAW16 container)\n",
		       isp_step_test_run_id, ISP_RAW_TO_Y_STEP_COUNT,
		       isp_step_test_min_raw_dn,
		       isp_step_test_max_raw_dn);
		pthread_mutex_unlock(&g_manual_calibration_lock);
		correction_lock_held = 0;
		goto after_corrections;
	}

	/* Corrections: VCAP -> optional TDC -> optional NUC -> optional FFC. */
	if (!g_manual_calibration_active && tdc_is_enabled()) {
		ret = tdc_apply(current_frame,
				&tdc_video_frame,
				p_stream0->thermal_tdc_out_pa,
				p_stream0->thermal_tdc_out_blk,
				RAW_SIZE_W,
				RAW_SIZE_H);
		if (ret != HD_OK) {
			printf("TDC correction error=%d !!\r\n\r\n", ret);
			goto release_vcap_frame;
		}

		current_frame = &tdc_video_frame;
		current_frame_is_vcap = 0;
	}

	if (!g_manual_calibration_active && nuc_is_enabled()) {
		if (nuc_prepare_blackbody_dynamic_table(current_frame) != 0) {
			printf("Dynamic NUC table build failed; bypass NUC for this frame\n");
		} else {
			ret = nuc_apply(current_frame,
					&nuc_video_frame,
					p_stream0->thermal_k_frame_pa,
					p_stream0->thermal_b_frame_pa,
					p_stream0->thermal_nuc_out_pa,
					p_stream0->thermal_nuc_out_blk,
					RAW_SIZE_W,
					RAW_SIZE_H);
			if (ret != HD_OK) {
				printf("nuc correction error=%d !!\r\n\r\n", ret);
				goto release_vcap_frame;
			}

			current_frame = &nuc_video_frame;
			current_frame_is_vcap = 0;
			nuc_applied = 1;
		}
	}

	if (!g_manual_calibration_active &&
	    ffc_is_enabled() && ffc_table_is_ready()) {
		ret = ffc_apply(current_frame,
				&ffc_video_frame,
				p_stream0->thermal_ffc_k_frame_pa,
				p_stream0->thermal_ffc_b_frame_pa,
				p_stream0->thermal_ffc_out_pa,
				p_stream0->thermal_ffc_out_blk,
				RAW_SIZE_W,
				RAW_SIZE_H);
		if (ret != HD_OK) {
			printf("ffc correction error=%d !!\r\n\r\n", ret);
			goto release_vcap_frame;
		}

		current_frame = &ffc_video_frame;
		current_frame_is_vcap = 0;
	}

	/* Claim the request while still holding the correction lock, so a frame
	 * processed before calibration completed cannot consume the new request.
	 * If the writer is busy, keep it pending for a later corrected frame.
	 */
	if (g_nuc_snapshot_pending && nuc_applied &&
	    !p_stream0->cap_snap && snapshot_writer_reserve_request() == 0) {
		g_nuc_snapshot_pending = 0;
		snapshot_requested = 1;
		snapshot_final_is_input = current_frame_is_vcap;
		printf("NUC automatic snapshot: corrected frame ready\n");
	}
	if (!g_manual_calibration_active && !raw_copy_handled &&
	    !snapshot_requested && !p_stream0->cap_snap &&
	    (timed_snapshot_job = timed_snapshot_reserve_request()) != 0) {
		snapshot_requested = 1;
		snapshot_final_is_input = current_frame_is_vcap;
	}
	pthread_mutex_unlock(&g_manual_calibration_lock);
	correction_lock_held = 0;

after_corrections:
	snapshot_final_frame = current_frame;
	if (!isp_step_test_requested && p_stream0->cap_snap) {
		p_stream0->cap_snap = 0;
		snapshot_requested = 1;
		snapshot_final_frame = current_frame;
		snapshot_final_is_input = current_frame_is_vcap;
	}

	/*
	 * Display enhancement runs after the correction snapshot point.  A
	 * pending request retains both the pre-AGC frame and, when available,
	 * the selected CPU AGC output before either one is submitted to VideoProc.
	 */
	if (!isp_step_test_requested && agc_histogram_p1_p99_is_enabled()) {
		agc_start_ms = liveview_get_time_ms();
		agc_status = liveview_apply_agc_histogram_p1_p99(
			p_stream0,
			current_frame,
			&agc_histogram_p1_p99_video_frame,
			&agc_p1,
			&agc_p99);
		agc_end_ms = liveview_get_time_ms();
		agc_elapsed_ms = agc_end_ms >= agc_start_ms ?
			agc_end_ms - agc_start_ms : 0;

		if (agc_status >= AGC_HISTOGRAM_P1_P99_STATUS_OK) {
			p_stream0->agc_histogram_p1_p99_frame_count++;
			if (p_stream0->agc_histogram_p1_p99_frame_count == 1U) {
				printf("Histogram P1/P99 AGC (CPU): frame=%u, "
				       "P1=%u, P99=%u, AGC RAW14 output=0..16383, "
				       "%s, elapsed=%" PRIu64 " ms\n",
				       p_stream0->agc_histogram_p1_p99_frame_count,
				       (unsigned int)agc_p1,
				       (unsigned int)agc_p99,
				       agc_status == AGC_HISTOGRAM_P1_P99_STATUS_FLAT_RANGE ?
					"flat-range copy" : "linear stretch",
				       (uint64_t)agc_elapsed_ms);
			}

			current_frame = &agc_histogram_p1_p99_video_frame;
			current_frame_is_vcap = 0;
			if (!isp_step_test_requested) {
				snapshot_agc_output_frame = current_frame;
				snapshot_has_agc_output = 1;
			}
		} else {
			p_stream0->agc_histogram_p1_p99_error_count++;
			if (p_stream0->agc_histogram_p1_p99_error_count == 1U) {
				printf("Histogram P1/P99 AGC (CPU) bypass: "
				       "error=%d, count=%u\n",
				       agc_status,
				       p_stream0->agc_histogram_p1_p99_error_count);
			}
		}
	}

	if (!isp_step_test_requested && agc_fieldscale_is_enabled()) {
		agc_start_ms = liveview_get_time_ms();
		agc_status = liveview_apply_agc_fieldscale(
			p_stream0,
			current_frame,
			&agc_histogram_p1_p99_video_frame);
		agc_end_ms = liveview_get_time_ms();
		agc_elapsed_ms = agc_end_ms >= agc_start_ms ?
			agc_end_ms - agc_start_ms : 0;

		if (agc_status == AGC_FIELDSCALE_STATUS_OK) {
			p_stream0->agc_fieldscale_frame_count++;
			if (p_stream0->agc_fieldscale_frame_count == 1U) {
				printf("Fieldscale Local Adaptive AGC (CPU): "
				       "frame=%u, grid=8x8, iterations=7, "
				       "temporal=current10%%/previous90%%, "
				       "AGC RAW14 output=0..16383, "
				       "elapsed=%" PRIu64 " ms\n",
				       p_stream0->agc_fieldscale_frame_count,
				       (uint64_t)agc_elapsed_ms);
			}
			current_frame = &agc_histogram_p1_p99_video_frame;
			current_frame_is_vcap = 0;
			if (!isp_step_test_requested) {
				snapshot_agc_output_frame = current_frame;
				snapshot_has_agc_output = 1;
			}
		} else {
			p_stream0->agc_fieldscale_error_count++;
			if (p_stream0->agc_fieldscale_error_count == 1U) {
				printf("Fieldscale Local Adaptive AGC (CPU) bypass: "
				       "error=%d, count=%u\n",
				       agc_status,
				       p_stream0->agc_fieldscale_error_count);
			}
		}
	}

	if (!isp_step_test_requested &&
	    agc_min_max_tone_curve_is_enabled()) {
		agc_start_ms = liveview_get_time_ms();
		agc_status = liveview_apply_agc_min_max_tone_curve(
			current_frame, &agc_tone_curve_lut);
		agc_end_ms = liveview_get_time_ms();
		agc_elapsed_ms = agc_end_ms >= agc_start_ms ?
			agc_end_ms - agc_start_ms : 0;

		if (agc_status >= AGC_MIN_MAX_TONE_CURVE_STATUS_OK) {
			p_stream0->agc_min_max_tone_curve_frame_count++;
			if (p_stream0->agc_min_max_tone_curve_frame_count == 1U) {
				printf("Min/Max Tone Curve AGC (ISP): frame=%u, "
				       "RAW14 min=%u, max=%u, Tone12 min=%u, max=%u, "
				       "%s, elapsed=%" PRIu64 " ms\n",
				       p_stream0->agc_min_max_tone_curve_frame_count,
				       (unsigned int)agc_tone_curve_lut.raw14_min,
				       (unsigned int)agc_tone_curve_lut.raw14_max,
				       (unsigned int)agc_tone_curve_lut.tone12_min,
				       (unsigned int)agc_tone_curve_lut.tone12_max,
				       agc_status ==
					AGC_MIN_MAX_TONE_CURVE_STATUS_FLAT_RANGE ?
					"flat-range zero LUT" : "manual tone LUT",
				       (uint64_t)agc_elapsed_ms);
			}
		} else {
			p_stream0->agc_min_max_tone_curve_error_count++;
			if (p_stream0->agc_min_max_tone_curve_error_count == 1U) {
				printf("Min/Max Tone Curve AGC (ISP) failed: "
				       "keep previous ISP tone curve, error=%d, count=%u\n",
				       agc_status,
				       p_stream0->agc_min_max_tone_curve_error_count);
			}
		}
	}

	if (snapshot_requested &&
	    queue_pipeline_snapshot(&vcap_video_frame,
				    snapshot_final_frame,
				    snapshot_final_is_input,
				    snapshot_agc_output_frame,
				    snapshot_has_agc_output,
				    timed_snapshot_job, capture_unix_ms) != 0) {
		printf("Snapshot frame queue failed\n");
	}

	if (!isp_step_test_requested && !raw_copy_handled) {
		pthread_mutex_lock(&g_manual_calibration_lock);
		if (!g_manual_calibration_active)
			raw_recording_queue(&vcap_video_frame, snapshot_final_frame,
				snapshot_agc_output_frame, capture_unix_ms);
		pthread_mutex_unlock(&g_manual_calibration_lock);
	}

	/* A processed frame no longer depends on the held VCAP buffer. */
	if (!current_frame_is_vcap) {
		ret = hd_videocap_release_out_buf(
			p_stream0->cap_path, &vcap_video_frame);
		if (ret != HD_OK) {
			printf("cap_release error=%d !!\r\n\r\n", ret);
			if (isp_step_test_requested) {
				liveview_finish_isp_step_test(-1);
			}
			return;
		}
	}

	if (!g_proc_input_format_logged) {
		printf("VideoProc input format: frame=0x%08X, configured=0x%08X\n",
		       current_frame->pxlfmt, CAP_OUT_FMT);
		g_proc_input_format_logged = 1;
	}
	if (isp_step_test_requested) {
		liveview_set_isp_step_test_expected(
			isp_step_test_run_id,
			current_frame->timestamp,
			current_frame->count);
	}
	push_ret = hd_videoproc_push_in_buf(
		p_stream0->proc_path, current_frame, NULL, 0);

	/* Direct VCAP routing must push before releasing the source buffer. */
	if (current_frame_is_vcap) {
		ret = hd_videocap_release_out_buf(
			p_stream0->cap_path, &vcap_video_frame);
		if (ret != HD_OK) {
			printf("cap_release error=%d !!\r\n\r\n", ret);
			return;
		}
	}

	if (push_ret != HD_OK) {
		printf("proc_push error=%d !!\r\n\r\n", push_ret);
		if (isp_step_test_requested) {
			liveview_finish_isp_step_test(-1);
		}
		return;
	}

	/* Do not delay the next calibration RAW request with runtime skipping. */
	if (!raw_copy_handled && g_runtime_skip_frames > 0) {
		skip_vcap_frame(p_stream0, g_runtime_skip_frames);
	}
	return;

release_vcap_frame:
	if (correction_lock_held) {
		pthread_mutex_unlock(&g_manual_calibration_lock);
	}
	ret = hd_videocap_release_out_buf(
		p_stream0->cap_path, &vcap_video_frame);
	if (ret != HD_OK) {
		printf("cap_release error=%d !!\r\n\r\n", ret);
	}
}

/* Called by the VCAP owner only. Drop queued frames, then four fresh frames,
 * so the final stage does not consume frames acquired before the ISP switch. */
static int raw_recording_settle_bpr(VIDEO_LIVEVIEW *stream)
{
	HD_VIDEO_FRAME frame = {0};
	HD_RESULT ret;
	UINT32 i, tries;
	for (i = 0; i < 64U; i++) {
		if (stream->cap_exit) return -1;
		ret = hd_videocap_pull_out_buf(stream->cap_path, &frame, 0);
		if (ret == HD_ERR_UNDERRUN) break;
		if (ret != HD_OK) return -1;
		if (hd_videocap_release_out_buf(stream->cap_path, &frame) != HD_OK) return -1;
	}
	if (i == 64U) return -1;
	for (i = 0; i < 4U; i++) {
		for (tries = 0; tries < 20U; tries++) {
			if (stream->cap_exit) return -1;
			ret = hd_videocap_pull_out_buf(stream->cap_path, &frame, 100);
			if (ret != HD_ERR_UNDERRUN) break;
		}
		if (ret != HD_OK) return -1;
		if (hd_videocap_release_out_buf(stream->cap_path, &frame) != HD_OK) return -1;
	}
	return 0;
}

/* No lifecycle lock: Stop owns it while joining this worker. Lock order is
 * correction -> writer, matching the normal RAW processing path. No CFG write. */
static void raw_recording_service_bpr(VIDEO_LIVEVIEW *stream)
{
	int restoring = 0, desired = 0, work = 0, ret;
	pthread_mutex_lock(&g_manual_calibration_lock);
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (!g_raw_recording.job && !g_raw_recording.control_busy) {
		if (g_raw_recording.restore_pending && !g_raw_recording.active) {
			restoring = work = 1;
			desired = g_raw_recording.initial_bpr;
		} else if (g_raw_recording.active && g_raw_recording.transition_pending &&
		           !g_manual_calibration_active) {
			work = 1;
		}
	}
	if (work) g_raw_recording.control_busy = 1;
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	if (!work) {
		pthread_mutex_unlock(&g_manual_calibration_lock);
		return;
	}
	ret = bpr_set_enabled(desired);
	if (ret >= 0 && (!restoring || !stream->cap_exit))
		ret = raw_recording_settle_bpr(stream);
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (ret < 0) {
		raw_recording_stop_locked(restoring ? "BPR restore failed" : "BPR switch/settle failed");
		printf("RAW recording BPR %s failed; restoration remains pending\n",
			restoring ? "restore" : "switch");
	} else if (restoring) {
		g_raw_recording.restore_pending = 0;
		g_raw_recording.transition_pending = 0;
		printf("RAW recording restored initial BPR=%s\n", desired ? "on" : "off");
	} else {
		g_raw_recording.transition_pending = 0;
		printf("RAW recording: BPR off, stale frames discarded; starting final Original stage\n");
	}
	g_raw_recording.control_busy = 0;
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	pthread_mutex_unlock(&g_manual_calibration_lock);
}

static void *raw_pipeline_thread(void *arg)
{
	VIDEO_LIVEVIEW* p_stream0 = (VIDEO_LIVEVIEW *)arg;
	UINT64 tdc_next_update_ms = 0;
	UINT64 nuc_sensor_temp_next_update_ms = 0;
	int shutter_calibration_enabled;
	int shutter_calibration_result;
	int shutter_calibration_paused;
	int startup_ffc_pending;

	//------ wait flow_start ------
	while (p_stream0->cap_enter == 0 && p_stream0->cap_exit == 0) usleep(100);
	if (p_stream0->cap_exit) {
		liveview_set_ffc_test_worker_active(0);
		return 0;
	}

	// Skip unstable frames once when the liveview pipeline starts.
	printf("startup skip %u frame ++\r\n", g_startup_skip_frames);
	skip_vcap_frame(p_stream0, g_startup_skip_frames);
	printf("startup skip %u frame --\r\n", g_startup_skip_frames);

	shutter_calibration_enabled =
		ffc_is_enabled() && ffc_get_shutter_interval_sec() > 0;
	g_shutter_next_calibration_ms = 0;
	g_shutter_reschedule_after_manual_calibration = 0;
	/* Startup calibration is independent of continuous FFC correction. */
	startup_ffc_pending = ffc_is_startup_enabled();
	if (startup_ffc_pending) {
		printf("Startup FFC calibration requested\n");
	} else if (shutter_calibration_enabled) {
		liveview_schedule_next_shutter_calibration();
	}

	while (p_stream0->cap_exit == 0) {
		raw_recording_service_bpr(p_stream0);
		pthread_mutex_lock(&g_snapshot_writer.lock);
		minute_raw_expire(&g_minute_raw, liveview_get_time_ms());
		pthread_mutex_unlock(&g_snapshot_writer.lock);
		shutter_calibration_paused =
			liveview_prepare_shutter_interval(
				&shutter_calibration_enabled);

		if (!shutter_calibration_paused &&
		    liveview_take_ffc_test_request()) {
			shutter_calibration_result =
				liveview_run_shutter_calibration_cycle(
					p_stream0,
					&nuc_sensor_temp_next_update_ms,
					&tdc_next_update_ms,
					1, 0);
			liveview_finish_ffc_test(shutter_calibration_result);
			if (shutter_calibration_enabled) {
				liveview_schedule_next_shutter_calibration();
			}
			usleep(100);
			continue;
		}

		liveview_update_temperature_tables_if_needed(
			&nuc_sensor_temp_next_update_ms,
			&tdc_next_update_ms);

		if (!shutter_calibration_paused &&
		    (startup_ffc_pending ||
		     (shutter_calibration_enabled &&
		      liveview_get_time_ms() >= g_shutter_next_calibration_ms))) {
			shutter_calibration_result =
				liveview_run_shutter_calibration_cycle(
					p_stream0,
					&nuc_sensor_temp_next_update_ms,
					&tdc_next_update_ms,
					0, startup_ffc_pending);
			if (shutter_calibration_result < 0) {
				g_shutter_next_calibration_ms = 0;
				break;
			}

			if (shutter_calibration_result == 0) {
				startup_ffc_pending = 0;
				if (shutter_calibration_enabled) {
					liveview_schedule_next_shutter_calibration();
				}
			}
			usleep(100);
			continue;
		}

		liveview_process_live_frame(p_stream0);
		usleep(100);
	}

	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_stop_locked("RAW pipeline stopped");
	minute_raw_stop(&g_minute_raw, "RAW pipeline stopped");
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	raw_recording_service_bpr(p_stream0);
	liveview_set_ffc_test_worker_active(0);
	return 0;
}

static void *yuv_statistics_thread(void *arg)
{
	VIDEO_LIVEVIEW *p_stream0 = (VIDEO_LIVEVIEW *)arg;
	HD_VIDEO_FRAME proc_frame = {0};
	HD_RESULT ret;

	while (p_stream0->proc_enter == 0 && p_stream0->proc_exit == 0) {
		usleep(100);
	}
	while (p_stream0->proc_exit == 0) {
		ret = hd_videoproc_pull_out_buf(
			p_stream0->proc_path, &proc_frame, 100);
		if (ret == HD_OK) {
			HD_VIDEO_FRAME mirrored = {0};
			HD_VIDEO_FRAME *display = &proc_frame;
			int have_mirror = 0;
			int display_ok = 1;

			/* Inspect OUT_0 before display processing; no extra output path. */
			process_requested_isp_step_test_output(p_stream0, &proc_frame);
			print_requested_yuv_frame_stats(&proc_frame);
			if (g_liveview_mirror) {
				ret = vpe_mirror_process(&proc_frame, &mirrored);
				if (ret != HD_OK) {
					printf("VPE mirror process failed=%d; stopping display worker\n", ret);
					if (vpe_mirror_input_pending()) {
						p_stream0->vpe_held_input = proc_frame;
						p_stream0->vpe_input_held = 1;
					} else {
						hd_videoproc_release_out_buf(p_stream0->proc_path, &proc_frame);
					}
					break;
				}
				display = &mirrored;
				have_mirror = 1;
			}
			if (clahe_process_frame(display) != 0) {
				printf("CLAHE processing failed; frame dropped\n");
				display_ok = 0;
			}
			if (display_ok && dde_process_frame(display) != 0) {
				printf("DDE processing failed; frame dropped\n");
				display_ok = 0;
			}
			if (display_ok && PseudoColor_YUV420(display) != 0) {
				printf("PseudoColor YUV processing failed; frame dropped\n");
				display_ok = 0;
			}
			if (display_ok) {
				ret = video_output_push_frame(&p_stream0->video_output, display);
				if (ret != HD_OK) printf("videoenc push error=%d\n", ret);
			}
			if (have_mirror) {
				ret = vpe_mirror_release(&mirrored);
				if (ret != HD_OK) printf("VPE output release failed=%d\n", ret);
			}
			ret = hd_videoproc_release_out_buf(p_stream0->proc_path, &proc_frame);
			if (ret != HD_OK) printf("proc_release error=%d\n", ret);
		} else if (ret != HD_ERR_UNDERRUN && ret != HD_ERR_TIMEDOUT) {
			printf("proc_pull error=%d !!\r\n\r\n", ret);
		}
	}

	return 0;
}

static int liveview_start_pipeline(LIVEVIEW_CONTEXT *ctx)
{
	HD_RESULT ret = HD_OK;
	VIDEO_LIVEVIEW *stream = ctx->stream;
	HD_DIM main_dim;
	ISPT_C_GAIN cgain = {0};
	IQT_CFG_INFO cfg_info = {0};
	int isp_cfg_loaded = 0;
	NUC_IMAGE_LAYOUT nuc_layout = {
		.raw_width = RAW_SIZE_W,
		.raw_height = RAW_SIZE_H,
		.active_x = RAW_ACTIVE_X,
		.active_y = RAW_ACTIVE_Y,
		.active_width = RAW_ACTIVE_W,
		.active_height = RAW_ACTIVE_H,
	};
	TDC_IMAGE_LAYOUT tdc_layout = {
		.raw_width = RAW_SIZE_W,
		.raw_height = RAW_SIZE_H,
		.active_x = RAW_ACTIVE_X,
		.active_y = RAW_ACTIVE_Y,
		.active_width = RAW_ACTIVE_W,
		.active_height = RAW_ACTIVE_H,
	};

	memset(stream, 0, sizeof(ctx->stream));
	ctx->result = 0;

	// init hdal
	ret = hd_common_init(0);
	if (ret != HD_OK) {
		printf("common fail=%d\n", ret);
		goto exit;
	}
	ctx->common_inited = 1;

	// init memory
	ret = mem_init();
	if (ret != HD_OK) {
		printf("mem fail=%d\n", ret);
		goto exit;
	}
	ctx->mem_inited = 1;

	// init all modules
	ret = init_module();
	if (ret != HD_OK) {
		printf("init fail=%d\n", ret);
		goto exit;
	}
	ctx->modules_inited = 1;

	ret = vendor_isp_init();
	if (ret != HD_OK) {
		printf("vendor_isp_init failed=%d\n", ret);
		goto exit;
	}
	ctx->isp_inited = 1;

	cgain.id = g_thermal_pipeline_id;
	cgain.gain[0] = 256;
	cgain.gain[1] = 256;
	cgain.gain[2] = 256;
	vendor_isp_set_common(ISPT_ITEM_C_GAIN, &cgain);

	gamma_reset();
	cfg_info.id = g_thermal_pipeline_id;
	strncpy(cfg_info.path, THERMAL_ISP_CONFIG_PATH, CFG_NAME_LENGTH);
	ret = vendor_isp_set_iq(IQT_ITEM_RLD_CONFIG, &cfg_info);
	if (ret != HD_OK) {
		printf("ISP IQ reload failed=%d, path=%s\n", ret, cfg_info.path);
		ret = HD_OK;
	} else {
		printf("ISP IQ reload ok: %s\n", cfg_info.path);
		isp_cfg_loaded = 1;
	}

	ret = bpr_load_table();
	if (ret != 0) {
		printf("load BPR table from SD failed=%d; keep ISP cfg table\n", ret);
		ret = HD_OK;
	}

	if (isp_cfg_loaded) {
		ret = gamma_init_from_isp();
		if (ret != HD_OK) {
			printf("init Gamma from ISP failed=%d, keep ISP cfg Gamma\n",
			       ret);
			ret = HD_OK;
		}
	} else {
		printf("Gamma control unavailable because ISP cfg was not loaded: %s\n",
		       THERMAL_ISP_CONFIG_PATH);
	}

	if (gpio_shutter_open() != 0) {
		printf("open shutter before liveview start failed\n");
		liveview_set_shutter_fault(1);
		ret = HD_ERR_NG;
		goto exit;
	}
	liveview_set_shutter_fault(0);

	/* ##### Just a hard code to modify PAD driving, please use nvt-gpio.dtsi for your own project! #####
	For example
	&top {
		sgpio6 {pad_config = <PAD_DS_SGPIO(6) PAD_DS_L1 PAD_SGPIO(6) PAD_PULLUP>;};
	};
	*/
	system("mem w 0x2f0030160 0x01100011");


	ctx->thermal_frame_created = 1;
	ret = init_thermal_frame(&stream[0]);
	if (ret != HD_OK) {
		printf("init thermal frame failed\n");
		goto exit;
	}
	nuc_bind_runtime(stream[0].thermal_k_frame_va,
			 stream[0].thermal_b_frame_va,
			 VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT),
			 &nuc_layout);
	nuc_bind_capture_callback(liveview_copy_next_raw);
	nuc_bind_calibration_state_callback(
		liveview_on_manual_calibration_state);
	fos_bind_capture_callback(liveview_copy_next_raw,
				  RAW_ACTIVE_W, RAW_ACTIVE_H);
	fos_bind_calibration_state_callback(
		liveview_on_manual_calibration_state);
	bpr_bind_calibration_state_callback(
		liveview_on_manual_calibration_state);
	tdc_bind_runtime(&tdc_layout);
	tdc_bind_capture_callback(liveview_copy_next_raw);
	nuc_init_table();
	ffc_bind_runtime(stream[0].thermal_ffc_b_frame_va,
			 VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT));
	ffc_init_table();
	// init TDC memory
	ret = init_thermal_TDC_mem();		
	if (ret != HD_OK) {
		printf("init thermal mem failed\n");
		goto exit;
	}

	ret = app_config_load();
	if (ret != 0) {
		printf("apply CFG settings failed; check the messages above\n");
	}
	// open video_liveview modules (main)
	stream[0].proc_max_dim.w = VDO_SIZE_W; //assign by user
	stream[0].proc_max_dim.h = VDO_SIZE_H; //assign by user
	ctx->module_open_attempted = 1;
	ret = open_module(&stream[0], &stream[0].proc_max_dim);
	if (ret != HD_OK) {
		printf("open fail=%d\n", ret);
		goto exit;
	}
	g_diagnostic_video_output = &stream[0].video_output;

	// get videocap capability
	ret = get_cap_caps(stream[0].cap_ctrl, &stream[0].cap_syscaps);
	if (ret != HD_OK) {
		printf("get cap-caps fail=%d\n", ret);
		goto exit;
	}

	// set videocap parameter
	stream[0].cap_dim.w = SENSOR_MODE_W;
	stream[0].cap_dim.h = SENSOR_MODE_H;
	ret = set_cap_param(stream[0].cap_path, &stream[0].cap_dim);
	if (ret != HD_OK) {
		printf("set cap fail=%d\n", ret);
		goto exit;
	}

	// VideoProc output is fixed because there is no VideoOut to provide a size.
	main_dim.w = RAW_ACTIVE_W;
	main_dim.h = RAW_ACTIVE_H;
	ret = set_proc_param(stream[0].proc_path, &main_dim,
		HD_VIDEO_PXLFMT_YUV420, 1);
	if (ret != HD_OK) {
		printf("set proc fail=%d\n", ret);
		goto exit;
	}

	/*
	 * A CPU AGC already stretches RAW14 before VideoProc. Bypass Tone Curve so
	 * the stretched frame is not remapped again.
	 * Save the configured tone before AGC overrides it so runtime OFF can
	 * restore it without restarting Liveview.
	 */
	memset(&g_agc_baseline_tone, 0, sizeof(g_agc_baseline_tone));
	g_agc_baseline_tone.id = g_thermal_pipeline_id;
	g_agc_baseline_valid = vendor_isp_get_iq(
		IQT_ITEM_TONE_PARAM, &g_agc_baseline_tone) == HD_OK;
	agc_mode_begin_session();
	ret = liveview_bypass_isp_tone_for_cpu_agc();
	if (ret != HD_OK) {
		printf("%s: Tone Curve bypass unavailable; "
		       "disable CPU AGC for this Liveview run\n",
		       agc_mode_name(agc_mode_get()));
		agc_mode_set(AGC_MODE_OFF);
		ret = HD_OK;
	}

	if (stream[0].proc_path_3dnr != 0) {
		ret = set_proc_param(stream[0].proc_path_3dnr, &main_dim,
			HD_VIDEO_PXLFMT_YUV420, 0);
		if (ret != HD_OK) {
			printf("set 3DNR reference proc fail=%d\n", ret);
			goto exit;
		}
	}
	ret = liveview_set_proc_input_direction(stream[0].proc_path);
	if (ret != HD_OK) {
		goto exit;
	}

	if (liveview_prepare_output_directories() != 0) {
		printf("prepare snapshot output directories failed\n");
		goto exit;
	}

	if (snapshot_writer_start() != 0) {
		printf("start snapshot background writer failed\n");
		goto exit;
	}
	ctx->snapshot_writer_started = 1;

	// create raw_pipeline_thread (VCAP -> TDC -> NUC -> FFC -> RAW snapshot -> selected AGC -> VideoProc)
	ret = pthread_create(&stream[0].cap_thread_id, NULL, raw_pipeline_thread, (void *)stream);
	if (ret != 0) {
		printf("create raw_pipeline_thread failed");
		goto exit;
	}
	ctx->cap_thread_created = 1;
	liveview_set_ffc_test_worker_active(1);
	ret = pthread_create(&stream[0].proc_thread_id, NULL,
		yuv_statistics_thread, (void *)stream);
	if (ret != 0) {
		printf("create yuv_statistics_thread failed\n");
		goto exit;
	}
	ctx->proc_thread_created = 1;

	if (g_liveview_mirror) {
		ret = vpe_mirror_start(RAW_ACTIVE_W, RAW_ACTIVE_H,
			DBGINFO_BUFSIZE() + VDO_YUV_BUFSIZE(RAW_ACTIVE_W,
				RAW_ACTIVE_H, HD_VIDEO_PXLFMT_YUV420));
		if (ret != HD_OK) goto exit;
	}
	ret = Pseudo_Init(RAW_ACTIVE_W * RAW_ACTIVE_H);
	if (ret != HD_OK) {
		printf("Pseudo init failed=%d\n", ret);
		goto exit;
	}
	ctx->pseudo_started = 1;
	printf("Pseudo color mode=%d (%s)\n", pseudo_get_mode(),
	       pseudo_mode_name(pseudo_get_mode()));

	// start video_liveview modules (main)
	ret = hd_videocap_start(stream[0].cap_path);
	if (ret != HD_OK) {
		printf("videocap start failed=%d\n", ret);
		goto exit;
	}
	ctx->cap_started = 1;

	ret = hd_videoproc_start(stream[0].proc_path);
	if (ret != HD_OK) {
		printf("videoproc start failed=%d\n", ret);
		goto exit;
	}
	ctx->proc_started = 1;

	if (stream[0].proc_path_3dnr != 0) {
		ret = hd_videoproc_start(stream[0].proc_path_3dnr);
		if (ret != HD_OK) {
			printf("videoproc 3DNR start failed=%d\n", ret);
			goto exit;
		}
		ctx->proc_3dnr_started = 1;
	}
	// wait thermal stable
	sleep(2);
	ret = video_output_start(&stream[0].video_output);
	if (ret != HD_OK) {
		printf("%s output start failed=%d\n",
			video_output_mode_name(g_output_mode), ret);
		goto exit;
	}

	// start raw_pipeline_thread
	stream[0].cap_enter = 1;
	stream[0].proc_enter = 1;

	ctx->running = 1;
	printf("Thermal %s liveview started.\n",
		video_output_mode_name(g_output_mode));
	return 0;

exit:
	ctx->result = ret;
	liveview_stop_pipeline(ctx);
	return -1;
}

static void liveview_stop_pipeline(LIVEVIEW_CONTEXT *ctx)
{
	VIDEO_LIVEVIEW *stream = ctx->stream;
	HD_RESULT ret;

	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_stop_locked("Liveview stopped");
	timed_snapshot_stop(&g_timed_snapshot, "Liveview stopped");
	minute_raw_stop(&g_minute_raw, "Liveview stopped");
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	g_diagnostic_video_output = NULL;
	/* Wake threads that may still be waiting for the pipeline to start. */
	stream[0].cap_exit = 1;
	stream[0].proc_exit = 1;
	liveview_finish_isp_step_test(-1);

	if (ctx->cap_thread_created) {
		pthread_join(stream[0].cap_thread_id, NULL);
		ctx->cap_thread_created = 0;
		stream[0].cap_thread_id = 0;
	}
	/* Drain the last count-recording job before any cleanup can return early.
	 * BPR restoration needs the ISP to remain initialized. */
	pthread_mutex_lock(&g_snapshot_writer.lock);
	while (g_raw_recording.job)
		pthread_cond_wait(&g_snapshot_writer.cond, &g_snapshot_writer.lock);
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	raw_recording_service_bpr(&stream[0]);
	liveview_finish_ffc_test(-1);
	if (ctx->proc_thread_created) {
		pthread_join(stream[0].proc_thread_id, NULL);
		ctx->proc_thread_created = 0;
		stream[0].proc_thread_id = 0;
	}
	clahe_frame_release();
	dde_frame_release();
	ret = vpe_mirror_stop();
	if (ret != HD_OK) {
		printf("VPE cleanup failed=%d; retain buffers, retry Liveview Stop\n", ret);
		return;
	}
	if (stream[0].vpe_input_held) {
		ret = hd_videoproc_release_out_buf(stream[0].proc_path,
			&stream[0].vpe_held_input);
		if (ret != HD_OK) {
			printf("VPE retained source release failed=%d\n", ret);
			return;
		}
		stream[0].vpe_input_held = 0;
	}
	if (ctx->pseudo_started) {
		ret = Pseudo_Uninit();
		if (ret != HD_OK) printf("Pseudo uninit failed=%d\n", ret);
		ctx->pseudo_started = 0;
	}
	if (ctx->snapshot_writer_started) {
		snapshot_writer_stop();
		ctx->snapshot_writer_started = 0;
	}
	/* A final queued RAW save may have kept BPR restoration pending until now. */
	raw_recording_service_bpr(&stream[0]);


	ret = video_output_stop(&stream[0].video_output);
	if (ret != HD_OK) printf("video output stop failed=%d\n", ret);

	if (ctx->cap_started) {
		ret = hd_videocap_stop(stream[0].cap_path);
		if (ret != HD_OK) printf("videocap stop failed=%d\n", ret);
		ctx->cap_started = 0;
	}
	if (ctx->proc_started) {
		ret = hd_videoproc_stop(stream[0].proc_path);
		if (ret != HD_OK) printf("videoproc stop failed=%d\n", ret);
		ctx->proc_started = 0;
	}
	if (ctx->proc_3dnr_started) {
		ret = hd_videoproc_stop(stream[0].proc_path_3dnr);
		if (ret != HD_OK) printf("videoproc 3DNR stop failed=%d\n", ret);
		ctx->proc_3dnr_started = 0;
	}
	if (ctx->proc_bound) {
		ret = hd_videoproc_unbind(HD_VIDEOPROC_0_OUT_0);
		if (ret != HD_OK) printf("video output unbind failed=%d\n", ret);
		ctx->proc_bound = 0;
	}
	if (ctx->module_open_attempted) {
		ret = close_module(&stream[0]);
		if (ret != HD_OK) printf("close module failed=%d\n", ret);
		ctx->module_open_attempted = 0;
	}

	nuc_bind_capture_callback(NULL);
	nuc_bind_calibration_state_callback(NULL);
	fos_bind_capture_callback(NULL, 0, 0);
	fos_bind_calibration_state_callback(NULL);
	bpr_bind_calibration_state_callback(NULL);
	nuc_bind_runtime(0, 0, 0, NULL);
	tdc_bind_capture_callback(NULL);
	ffc_bind_runtime(0, 0);
	g_raw_copy_pending = 0;
	g_raw_copy_done = 0;
	g_yuv_stats_pending = 0;
	g_proc_input_format_logged = 0;
	pthread_mutex_lock(&g_shutter_calibration_lock);
	pthread_mutex_lock(&g_manual_calibration_lock);
	g_shutter_next_calibration_ms = 0;
	g_manual_calibration_active = 0;
	g_nuc_snapshot_pending = 0;
	g_shutter_reschedule_after_manual_calibration = 0;
	pthread_mutex_unlock(&g_manual_calibration_lock);
	pthread_mutex_unlock(&g_shutter_calibration_lock);

	if (ctx->thermal_frame_created) {
		ret = exit_thermal_frame(&stream[0]);
		if (ret != HD_OK) printf("exit thermal frame fail=%d\n", ret);
		ctx->thermal_frame_created = 0;
	}
	ret = exit_TDC_mem();
	if (ret != HD_OK) {
		printf("exit TDC mem fail=%d\n", ret);
	}
	tdc_bind_runtime(NULL);

	if (ctx->modules_inited) {
		ret = exit_module();
		if (ret != HD_OK) printf("exit module failed=%d\n", ret);
		ctx->modules_inited = 0;
	}

	if (ctx->isp_inited) {
		ret = vendor_isp_uninit();
		if (ret != HD_OK) printf("vendor_isp_uninit failed=%d\n", ret);
		ctx->isp_inited = 0;
	}

	if (ctx->mem_inited) {
		ret = mem_exit();
		if (ret != HD_OK) printf("mem exit failed=%d\n", ret);
		ctx->mem_inited = 0;
	}

	if (ctx->common_inited) {
		ret = hd_common_uninit();
		if (ret != HD_OK) printf("common uninit failed=%d\n", ret);
		ctx->common_inited = 0;
	}

	memset(stream, 0, sizeof(ctx->stream));
	ctx->running = 0;
}

int liveview_init(void)
{
	if (g_liveview.initialized) {
		return 0;
	}

	g_liveview.running = 0;
	g_liveview.result = 0;
	g_liveview.initialized = 1;
	ffc_bind_test_callback(liveview_request_ffc_test);
	gpio_shutter_set_failure_callback(
		liveview_on_shutter_failure);
	pthread_mutex_lock(&g_liveview_fault_lock);
	g_liveview_shutter_fault = 0;
	g_liveview_stop_request = 0;
	g_liveview_fault_stop_worker_active = 0;
	pthread_mutex_unlock(&g_liveview_fault_lock);
	return 0;
}

int liveview_start(void)
{
	int ret;

	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (!g_liveview.initialized) {
		ret = -1;
		goto exit;
	}

	if (liveview_stop_is_requested()) {
		printf("Liveview stop is pending\n");
		ret = -1;
		goto exit;
	}

	if (g_liveview.running) {
		ret = 0;
		goto exit;
	}

	if (g_liveview.mem_inited) {
		printf("Previous pipeline cleanup incomplete; retry Stop first\n");
		ret = -1;
		goto exit;
	}
	ret = liveview_start_pipeline(&g_liveview);

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

void liveview_stop(void)
{
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (!g_liveview.initialized) {
		goto exit;
	}

	liveview_stop_pipeline(&g_liveview);

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
}

int liveview_restart(void)
{
	int ret;

	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (!g_liveview.initialized || liveview_stop_is_requested()) {
		ret = -1;
		goto exit;
	}

	liveview_stop_pipeline(&g_liveview);
	if (g_liveview.mem_inited) {
		printf("Previous pipeline cleanup incomplete; retry Stop first\n");
		ret = -1;
		goto exit;
	}
	ret = liveview_start_pipeline(&g_liveview);

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_is_running(void)
{
	return g_liveview.running;
}

static int liveview_set_correction_enabled(int is_bpr, int enabled)
{
	int ret = 1;

	/* Serialize with Start/Stop and with the RAW correction/calibration work. */
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (g_liveview.running) {
		pthread_mutex_lock(&g_manual_calibration_lock);
		if (is_bpr) {
			int recording_owns_bpr;
			pthread_mutex_lock(&g_snapshot_writer.lock);
			recording_owns_bpr = g_raw_recording.restore_pending;
			pthread_mutex_unlock(&g_snapshot_writer.lock);
			if (recording_owns_bpr) {
				printf("Stop sequence RAW recording before changing BPR\n");
				ret = -1;
			} else {
				ret = bpr_set_enabled(enabled);
			}
		} else {
			nuc_set_enabled(enabled);
			ret = nuc_is_enabled() == !!enabled ? 0 : -1;
		}
		pthread_mutex_unlock(&g_manual_calibration_lock);
	}
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_set_bpr_enabled(int enabled)
{
	return liveview_set_correction_enabled(1, enabled);
}

int liveview_set_nuc_enabled(int enabled)
{
	return liveview_set_correction_enabled(0, enabled);
}

int liveview_set_ffc_enabled(int enabled)
{
	int ret = 1;
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (g_liveview.running) {
		pthread_mutex_lock(&g_shutter_calibration_lock);
		pthread_mutex_lock(&g_manual_calibration_lock);
		pthread_mutex_lock(&g_ffc_test_lock);
		if (!enabled) {
			if (g_ffc_enable_after_test)
				g_ffc_enable_after_test = -1; /* Keep ownership, cancel auto-apply. */
			ffc_set_enabled(0);
			g_shutter_reschedule_after_manual_calibration = 1;
			ret = 0;
		} else if (g_ffc_enable_after_test) {
			g_ffc_enable_after_test = 1;
			ret = 2;
		} else if (ffc_table_is_ready()) {
			ffc_set_enabled(enabled);
			g_shutter_reschedule_after_manual_calibration = 1;
			ret = 0;
		} else if (g_ffc_test_worker_active) {
			ffc_set_enabled(0);
			g_ffc_enable_after_test = 1;
			if (!g_ffc_test_pending && !g_ffc_test_running) {
				g_ffc_test_result = -1;
				g_ffc_test_pending = 1;
			}
			printf("FFC enable requested; calibrating before applying compensation\n");
			ret = 2;
		} else {
			printf("FFC enable unavailable: calibration worker is not active\n");
			ret = -1;
		}
		pthread_mutex_unlock(&g_ffc_test_lock);
		pthread_mutex_unlock(&g_manual_calibration_lock);
		pthread_mutex_unlock(&g_shutter_calibration_lock);
	}
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_apply_shutter_close_wait_ms(unsigned int wait_ms)
{
	int ret = 1;
	if (wait_ms == 0 || wait_ms > INT_MAX) return -1;
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (g_liveview.running) {
		pthread_mutex_lock(&g_shutter_calibration_lock);
		liveview_set_shutter_close_wait_ms(wait_ms);
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		ret = 0;
	}
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_set_shutter_interval_sec(unsigned int interval_sec)
{
	int ret = 1;
	if (interval_sec > INT_MAX) return -1;
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (g_liveview.running) {
		pthread_mutex_lock(&g_shutter_calibration_lock);
		ffc_set_shutter_interval_sec(interval_sec);
		g_shutter_reschedule_after_manual_calibration = 1;
		if (interval_sec == 0)
			g_shutter_next_calibration_ms = 0;
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		ret = 0;
	}
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_get_result(void)
{
	return g_liveview.result;
}

int liveview_get_shutter_fault(void)
{
	int fault;

	pthread_mutex_lock(&g_liveview_fault_lock);
	fault = g_liveview_shutter_fault;
	pthread_mutex_unlock(&g_liveview_fault_lock);
	return fault;
}

int liveview_request_nuc_snapshot(void)
{
	int ret = -1;

	pthread_mutex_lock(&g_manual_calibration_lock);
	if (g_liveview.running && !g_manual_calibration_active &&
	    nuc_is_enabled()) {
		g_nuc_snapshot_pending = 1;
		printf("NUC automatic snapshot requested; waiting for a corrected frame\n");
		ret = 0;
	}
	pthread_mutex_unlock(&g_manual_calibration_lock);
	return ret;
}

int liveview_start_timed_snapshot(void)
{
	int ret = -1;

	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (!g_liveview.running || !g_snapshot_writer.thread_created ||
	    g_snapshot_writer.exit_requested || liveview_stop_is_requested()) {
		printf("Start Liveview before running Timed Snapshot\n");
	} else if (g_minute_raw.active || g_minute_raw.in_flight) {
		printf("Stop Minute RAW before running Timed Snapshot\n");
	} else if (g_raw_recording.active || g_raw_recording.job || g_raw_recording.restore_pending) {
		printf("Stop RAW recording before running Timed Snapshot\n");
	} else {
		ret = timed_snapshot_start(&g_timed_snapshot, RAW_SNAPSHOT_DIR,
			liveview_get_time_ms());
	}
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

void liveview_stop_timed_snapshot(void)
{
	pthread_mutex_lock(&g_snapshot_writer.lock);
	timed_snapshot_stop(&g_timed_snapshot, "user requested stop");
	pthread_mutex_unlock(&g_snapshot_writer.lock);
}

void liveview_show_timed_snapshot_status(void)
{
	pthread_mutex_lock(&g_snapshot_writer.lock);
	timed_snapshot_print_status(&g_timed_snapshot, liveview_get_time_ms());
	pthread_mutex_unlock(&g_snapshot_writer.lock);
}

static int liveview_request_snapshot(void)
{
	if (!g_liveview.running) {
		printf("Liveview is stopped\n");
		return -1;
	}
	if (g_liveview.stream[0].cap_snap) {
		printf("Snapshot request is already pending\n");
		return -1;
	}
	if (snapshot_writer_reserve_request() != 0) {
		printf("Snapshot background writer is busy\n");
		return -1;
	}

	g_liveview.stream[0].cap_snap = 1;
	return 0;
}

/* Applied from CFG before pipeline start; menu changes only pending CFG. */
void liveview_set_mirror(int enabled)
{
	g_liveview_mirror = enabled ? 1 : 0;
}

int liveview_get_mirror(void)
{
	return g_liveview_mirror;
}

int liveview_set_fps(unsigned int fps)
{
	if (fps == 0) {
		printf("FPS must be greater than 0\n");
		return -1;
	}

	g_fps = fps;
	return 0;
}

unsigned int liveview_get_fps(void)
{
	return g_fps;
}

int liveview_set_output_mode(unsigned int mode)
{
	if (!video_output_mode_is_valid(mode)) {
		printf("Output mode must be 0 (UVC) or 1 (RTSP)\n");
		return -1;
	}

	g_output_mode = (VIDEO_OUTPUT_MODE)mode;
	printf("Video output mode=%s\n",
		video_output_mode_name(g_output_mode));
	return 0;
}

unsigned int liveview_get_output_mode(void)
{
	return (unsigned int)g_output_mode;
}

void liveview_set_startup_skip_frames(unsigned int frame_count)
{
	g_startup_skip_frames = frame_count;
}

unsigned int liveview_get_startup_skip_frames(void)
{
	return g_startup_skip_frames;
}

void liveview_set_runtime_skip_frames(unsigned int frame_count)
{
	g_runtime_skip_frames = frame_count;
}

unsigned int liveview_get_runtime_skip_frames(void)
{
	return g_runtime_skip_frames;
}

void liveview_set_shutter_close_wait_ms(unsigned int wait_ms)
{
	if (wait_ms == 0) {
		printf("Shutter close wait time must be greater than 0 ms\n");
		return;
	}

	g_shutter_close_wait_ms = wait_ms;
	printf("Shutter close wait time=%u ms\n", g_shutter_close_wait_ms);
}

unsigned int liveview_get_shutter_close_wait_ms(void)
{
	return g_shutter_close_wait_ms;
}

static int liveview_read_option(void)
{
	char buf[32];
	char *end;
	unsigned long value;

	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == 0) {
		return -1;
	}

	if (buf[0] == 'q' || buf[0] == 'Q') {
		return -1;
	}

	value = strtoul(buf, &end, 0);
	if (end == buf) {
		if (buf[0] != '\n' && buf[0] != '\r') {
			printf("Invalid option\n");
		}
		return MENU_OPTION_REFRESH;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0' || value > INT_MAX) {
		printf("Invalid option\n");
		return MENU_OPTION_REFRESH;
	}

	return (int)value;
}

static int liveview_control_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Liveview control\n");
		printf(" 1. Start\n");
		printf(" 2. Stop\n");
		printf(" 3. Restart\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: Liveview=%s\n",
		       liveview_is_running() ? "running" : "stopped");

		option = liveview_read_option();
		switch (option) {
		case 1:
			ret = liveview_start();
			printf("Liveview start %s\n", ret == 0 ? "ok" : "fail");
			break;

		case 2:
			liveview_stop();
			ret = 0;
			printf("Liveview stopped\n");
			break;

		case 3:
			ret = liveview_restart();
			printf("Liveview restart %s\n", ret == 0 ? "ok" : "fail");
			break;

		case 0:
		case -1:
			running = 0;
			break;

		case MENU_OPTION_REFRESH:
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;
}

static int liveview_shutter_control_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Shutter control\n");
		printf(" 1. Open\n");
		printf(" 2. Close\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");

		option = liveview_read_option();
		switch (option) {
		case 1:
			ret = liveview_manual_shutter_command(1);
			printf("Shutter open %s\n", ret == 0 ? "ok" : "fail");
			break;

		case 2:
			ret = liveview_manual_shutter_command(0);
			printf("Shutter close %s\n", ret == 0 ? "ok" : "fail");
			break;

		case 0:
		case -1:
			running = 0;
			break;

		case MENU_OPTION_REFRESH:
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;
}

static int raw_recording_is_busy(void)
{
	int busy;
	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_check_limit_locked();
	busy = g_raw_recording.active || g_raw_recording.job || g_raw_recording.restore_pending;
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	return busy;
}

static void raw_recording_print_status(void)
{
	UINT64 elapsed;
	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_check_limit_locked();
	elapsed = g_raw_recording.started_ms ? liveview_get_time_ms() - g_raw_recording.started_ms : 0;
	printf("RAW Recording: %s, saved=%llu/%llu, skipped=%llu",
		g_raw_recording.active ? "Recording" : (g_raw_recording.job ? "Stopping" : "Stopped"),
		g_raw_recording.saved, g_raw_recording.target_frames *
			(g_raw_recording.type == 3 ? 3ULL : 1ULL), g_raw_recording.skipped);
	if (g_raw_recording.type == 3)
		printf(", stage=%d/3, stage_saved=%llu/%llu%s", g_raw_recording.phase + 1,
			g_raw_recording.phase_saved, g_raw_recording.target_frames,
			g_raw_recording.restore_pending && !g_raw_recording.active ? ", restoring BPR" : "");
	if (g_raw_recording.active) printf(", elapsed=%llu s", (unsigned long long)(elapsed / 1000));
	printf("\n");
	if (!g_raw_recording.active && g_raw_recording.reason)
		printf("Last stop: %s\n", g_raw_recording.reason);
	if (g_raw_recording.directory[0]) printf("Output: %s\n", g_raw_recording.directory);
	pthread_mutex_unlock(&g_snapshot_writer.lock);
}

static void raw_recording_toggle(int type, int frame_count)
{
#if defined(__LINUX)
	unsigned int attempt;
	char directory[192];
	int created = 0;
#endif
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	pthread_mutex_lock(&g_snapshot_writer.lock);
	raw_recording_check_limit_locked();
	if (g_raw_recording.active) {
		raw_recording_stop_locked("user requested stop");
		goto done;
	}
	if (g_raw_recording.job || g_raw_recording.restore_pending) {
		printf("RAW recording is finishing the last file / restoring BPR; please wait\n");
		goto done;
	}
	if (g_minute_raw.active || g_minute_raw.in_flight) {
		printf("Stop Minute RAW before starting RAW recording\n");
		goto done;
	}
	if (g_timed_snapshot.active || g_timed_snapshot.in_flight) {
		printf("Stop Timed Snapshot before starting RAW recording\n");
		goto done;
	}
	if (!g_liveview.running || !g_snapshot_writer.thread_created ||
	    g_snapshot_writer.exit_requested || liveview_stop_is_requested()) {
		printf("Start Liveview before recording\n");
		goto done;
	}
	if (type < 0 || type > 3 || frame_count <= 0) goto done;
	if (type == 2 && !agc_histogram_p1_p99_is_enabled() && !agc_fieldscale_is_enabled()) {
		printf("AGC output RAW requires Histogram P1/P99 or Fieldscale AGC\n");
		goto done;
	}
#if defined(__LINUX)
	if (!raw_recording_has_space(RAW_SNAPSHOT_DIR)) {
		printf("Cannot record: SD unavailable or less than 256 MiB plus one RAW frame free\n");
		goto done;
	}
	for (attempt = 0; attempt < 10000; attempt++) {
		snprintf(directory, sizeof(directory), RAW_SNAPSHOT_DIR "/recording_%lld_%04u",
			(long long)time(NULL), attempt);
		if (mkdir(directory, 0775) == 0) { created = 1; break; }
		if (errno != EEXIST) break;
	}
	if (!created) {
		printf("Cannot create recording directory: %s\n", strerror(errno));
		goto done;
	}
	snprintf(g_raw_recording.directory, sizeof(g_raw_recording.directory), "%s", directory);
	if (raw_recording_log_create(directory) != 0) {
		printf("Cannot create recording summary.csv\n");
		goto done;
	}
	g_raw_recording.type = type;
	g_raw_recording.phase = 0;
	g_raw_recording.phase_saved = 0;
	g_raw_recording.transition_pending = 0;
	g_raw_recording.control_busy = 0;
	g_raw_recording.initial_bpr = bpr_is_enabled();
	g_raw_recording.restore_pending = type == 3;
	g_raw_recording.started_ms = liveview_get_time_ms();
	g_raw_recording.target_frames = (unsigned long long)frame_count;
	g_raw_recording.saved = g_raw_recording.skipped = g_raw_recording.sequence = 0;
	g_raw_recording.reason = NULL;
	g_raw_recording.active = 1;
	pthread_cond_signal(&g_snapshot_writer.cond);
	printf("RAW recording started: %d images per stage, %d stage(s), output=%s\n",
		frame_count, type == 3 ? 3 : 1, directory);
#else
	printf("RAW recording requires Linux SD storage\n");
#endif
done:
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
}

static int liveview_raw_type_menu_run(int selected_raw_type)
{
	int option;

	for (;;) {
		printf("----------------------------------------\n");
		printf("Select RAW type\n");
		printf(" 1. Original RAW\n");
		printf(" 2. Corrected RAW\n");
		printf(" 3. AGC output RAW\n");
		printf(" 4. Original -> Corrected -> Original (BPR off)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		option = liveview_read_option();
		if (option >= 1 && option <= 4) {
			return option - 1;
		}
		if (option == 0 || option == -1) {
			return selected_raw_type;
		}
		if (option != MENU_OPTION_REFRESH) {
			printf("Invalid option\n");
		}
	}
}

static void liveview_minute_raw_toggle(unsigned int minutes)
{
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	pthread_mutex_lock(&g_snapshot_writer.lock);
	if (g_minute_raw.active) {
		minute_raw_stop(&g_minute_raw, "user requested stop");
	} else if (g_minute_raw.in_flight) {
		printf("Minute RAW is finishing the current pair; please wait\n");
	} else if (!g_liveview.running || !g_snapshot_writer.thread_created ||
	           g_snapshot_writer.exit_requested || liveview_stop_is_requested()) {
		printf("Start Liveview before running Minute RAW\n");
	} else if (g_raw_recording.active || g_raw_recording.job || g_raw_recording.restore_pending ||
	           g_timed_snapshot.active || g_timed_snapshot.in_flight) {
		printf("Stop Recording RAW / Timed Snapshot before running Minute RAW\n");
	} else if (!raw_recording_has_space(RAW_SNAPSHOT_DIR)) {
		printf("Cannot record: SD unavailable or less than 256 MiB reserve free\n");
	} else if (minute_raw_start(&g_minute_raw, RAW_SNAPSHOT_DIR,
	                           liveview_get_time_ms(), minutes) != 0) {
		printf("Cannot start Minute RAW; check SD storage\n");
	}
	pthread_mutex_unlock(&g_snapshot_writer.lock);
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
}

static int liveview_minute_raw_menu_run(void)
{
	static unsigned int minutes = 32;
	int option, busy;
	for (;;) {
		printf("----------------------------------------\n");
		printf("Minute Interval Recording: Original + Corrected (10 pairs/minute)\n");
		printf(" 1. Set total minutes (1..1440)\n");
		printf(" 2. Start / Stop\n");
		printf(" 3. Show status\n");
		printf(" 0. Back\n");
		printf("Total: %u min; first 10 pairs immediately, next batch every 60 s.\n", minutes);
		printf("Stops at duration; a queued pair finishes saving. Late/missed captures are not backfilled.\n");
		printf("Corrected RAW is before AGC. Temperature/time are recorded for each pair.\n");
		pthread_mutex_lock(&g_snapshot_writer.lock);
		minute_raw_expire(&g_minute_raw, liveview_get_time_ms());
		minute_raw_status(&g_minute_raw, liveview_get_time_ms());
		pthread_mutex_unlock(&g_snapshot_writer.lock);
		option = liveview_read_option();
		switch (option) {
		case 1:
			pthread_mutex_lock(&g_snapshot_writer.lock);
			busy = g_minute_raw.active || g_minute_raw.in_flight;
			pthread_mutex_unlock(&g_snapshot_writer.lock);
			if (busy) {
				printf("Stop Minute RAW before changing duration\n");
				break;
			}
			printf("Enter total minutes (1..1440, q to cancel)\n");
			option = liveview_read_option();
			if (option >= 1 && option <= 1440) minutes = (unsigned int)option;
			else printf("Duration unchanged\n");
			break;
		case 2:
			liveview_minute_raw_toggle(minutes);
			break;
		case 3:
		case MENU_OPTION_REFRESH:
			break;
		case 0:
		case -1:
			return 0;
		default:
			printf("Invalid option\n");
			break;
		}
	}
}

static int liveview_image_count_recording_menu_run(void)
{
	static int selected_raw_type = 0;
	static int recording_frame_count = 30;
	static const char * const raw_type_names[] = {
		"Original RAW", "Corrected RAW", "AGC output RAW",
		"Original -> Corrected -> Original (BPR off)"
	};
	int option;

	for (;;) {
		printf("----------------------------------------\n");
		printf("Image Count Recording\n");
		printf(" 1. Select RAW type\n");
		printf(" 2. Set recording image count\n");
		printf(" 3. Start / Stop recording\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Selected: %s\n", raw_type_names[selected_raw_type]);
		printf("Target: %d images per stage (%d stages)\n", recording_frame_count,
			selected_raw_type == 3 ? 3 : 1);
		raw_recording_print_status();
		printf("Each image records sensor temperature and capture time in summary.csv.\n");
		printf("Sequence mode: save N Original, N Corrected, then N Original with BPR off.\n");
		printf("BPR is restored to its initial state after sequence recording stops.\n");
		printf("AGC output RAW is available only in supported AGC modes.\n");

		option = liveview_read_option();
		switch (option) {
		case 1:
			if (raw_recording_is_busy()) {
				printf("Stop recording before changing RAW type\n");
				break;
			}
			selected_raw_type = liveview_raw_type_menu_run(selected_raw_type);
			break;
		case 2:
			if (raw_recording_is_busy()) {
				printf("Stop recording before changing image count\n");
				break;
			}
			printf("Enter recording image count (positive integer, q to cancel)\n");
			option = liveview_read_option();
			if (option > 0) {
				recording_frame_count = option;
			} else if (option != -1) {
				printf("Image count unchanged: enter a positive integer\n");
			}
			break;
		case 3:
			raw_recording_toggle(selected_raw_type, recording_frame_count);
			break;
		case 0:
		case -1:
			return 0;
		case MENU_OPTION_REFRESH:
			break;
		default:
			printf("Invalid option\n");
			break;
		}
	}
}

static int liveview_raw_recording_menu_run(void)
{
	int option;
	for (;;) {
		printf("----------------------------------------\n");
		printf("Recording RAW\n");
		printf(" 1. Image Count Recording\n");
		printf(" 2. Minute Interval Recording\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Image Count: select a RAW type or 3-stage sequence and set images per stage.\n");
		printf("Minute Interval: save 10 Original + Corrected pairs/minute with time and temperature.\n");
		printf("Select a mode to configure duration/count and start or stop recording.\n");
		option = liveview_read_option();
		switch (option) {
		case 1:
			liveview_image_count_recording_menu_run();
			break;
		case 2:
			liveview_minute_raw_menu_run();
			break;
		case 0:
		case -1:
			return 0;
		case MENU_OPTION_REFRESH:
			break;
		default:
			printf("Invalid option\n");
			break;
		}
	}
}

int liveview_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Liveview menu\n");
		printf(" 1. Status\n");
		printf(" 2. Liveview control\n");
		printf(" 3. Shutter control\n");
		printf(" 4. Snapshot RAW\n");
		printf(" 5. Recording RAW\n");
		printf(" 6. Show current YUV statistics\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Settings: CFG Setting -> Liveview setting\n");
		raw_recording_print_status();

		option = liveview_read_option();
		switch (option) {
		case 1:
			printf("Liveview: %s, output=%s, result=%d, shutter_fault=%s, fps=%u, startup_skip=%u, runtime_skip=%u\n",
			       liveview_is_running() ? "running" : "stopped",
			       video_output_mode_name(g_output_mode),
			       liveview_get_result(),
			       liveview_get_shutter_fault() ? "yes" : "no", g_fps,
			       g_startup_skip_frames, g_runtime_skip_frames);
			break;

		case 2:
			ret = liveview_control_menu_run();
			break;

		case 3:
			ret = liveview_shutter_control_menu_run();
			break;

		case 4:
			ret = liveview_request_snapshot();
			printf("Snapshot request %s\n", ret == 0 ? "ok" : "fail");
			break;

		case 5:
			ret = liveview_raw_recording_menu_run();
			break;

		case 6:
			ret = liveview_request_yuv_statistics();
			break;

		case 0:
		case -1:
			running = 0;
			break;

		case MENU_OPTION_REFRESH:
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;
}

void liveview_deinit(void)
{
	pthread_mutex_lock(&g_liveview_lifecycle_lock);
	if (!g_liveview.initialized) {
		goto exit;
	}

	liveview_stop_pipeline(&g_liveview);
	ffc_bind_test_callback(NULL);
	gpio_shutter_set_failure_callback(NULL);
	g_liveview.initialized = 0;

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
}
