/**
	@brief Sample code of video liveview with thermal sensor.\n

	@file liveview.c

	@author Willy Guo

	@ingroup mhdal

	@note This file is modified from video_liveview_with_mode.c.

	Copyright Novatek Microelectronics Corp. 2024.  All rights reserved.
*/

#include "liveview.h"

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
#endif
#include "hdal.h"
#include "vendor_videocapture.h"
#include "vendor_videoprocess.h"
#include "../Config/app_config.h"
#include "../Config/hardware_config.h"
#include "vendor_isp.h"
#include "../Algorithm/BPR/bpr.h"
#include "../Algorithm/FFC/ffc.h"
#include "../Algorithm/NUC/nuc.h"
#include "../Algorithm/TDC/tdc.h" 
#include "../Enhancement/Gamma/gamma.h"
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
#define RAW_STATS_WAIT_TIMEOUT_MS     1000U
#define YUV_STATS_WAIT_TIMEOUT_MS     1000U

#define SNAPSHOT_DIR                  "/mnt/sd"
#define SNAPSHOT_PREFIX               "snapshot_"

#define FFC_AVERAGE_FRAME_COUNT 4U
#define FFC_AVERAGE_SHIFT       2U

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
#define VIDEOPROC_ALG_FUNC HD_VIDEOPROC_FUNC_AF | HD_VIDEOPROC_FUNC_WDR | HD_VIDEOPROC_FUNC_DEFOG | HD_VIDEOPROC_FUNC_3DNR | HD_VIDEOPROC_FUNC_3DNR_STA

static UINT32 g_fps = 30;
static VIDEO_OUTPUT_MODE g_output_mode = VIDEO_OUTPUT_MODE_UVC;

static UINT32 g_startup_skip_frames = DEFAULT_STARTUP_SKIP_FRAMES;
/* VCAP frames dropped after each frame successfully submitted to VideoProc. */
static UINT32 g_runtime_skip_frames = DEFAULT_RUNTIME_SKIP_FRAMES;
static UINT32 g_shutter_close_wait_ms = DEFAULT_SHUTTER_CLOSE_WAIT_MS;

static volatile int g_raw_copy_pending = 0;
static volatile int g_raw_copy_done = 0;
static volatile int g_raw_copy_result = 0;
static UINTPTR g_raw_copy_dst_va = 0;
static UINT32 g_raw_copy_size = 0;
static volatile int g_raw_stats_pending = 1;
static volatile int g_yuv_stats_pending = 0;
static int g_proc_input_format_logged = 0;
static UINT64 g_shutter_next_calibration_ms = 0;
static int g_manual_calibration_active = 0;
static int g_shutter_reschedule_after_manual_calibration = 0;
///////////////////////////////////////////////////////////////////////////////

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

static int liveview_request_raw_statistics(void)
{
	UINT32 wait_count = 0;

	if (!liveview_is_running()) {
		printf("RAW statistics unavailable: Liveview is stopped\n");
		return -1;
	}

	if (g_raw_stats_pending) {
		printf("RAW statistics request is already pending\n");
	} else {
		printf("RAW statistics requested for the next VCAP input frame\n");
		g_raw_stats_pending = 1;
	}

	while (g_raw_stats_pending && wait_count < RAW_STATS_WAIT_TIMEOUT_MS) {
		usleep(1000);
		wait_count++;
	}
	if (g_raw_stats_pending) {
		printf("RAW statistics are still pending; waiting for a VCAP frame\n");
	}

	return 0;
}

static int liveview_request_yuv_statistics(void)
{
	UINT32 wait_count = 0;

	if (!liveview_is_running()) {
		printf("YUV statistics unavailable: Liveview is stopped\n");
		return -1;
	}

	if (g_yuv_stats_pending) {
		printf("YUV statistics request is already pending\n");
	} else {
		printf("YUV statistics requested for the next VideoProc output frame\n");
		g_yuv_stats_pending = 1;
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
		interval_sec = 1;
	}
	g_shutter_next_calibration_ms =
		liveview_get_time_ms() + (UINT64)interval_sec * 1000;
}

static void liveview_run_runtime_bpr_while_shutter_closed(void)
{
	int runtime_bpr_ret;

	if (!bpr_is_runtime_enabled()) {
		return;
	}

	runtime_bpr_ret = bpr_run_runtime_calibration();
	if (runtime_bpr_ret != 0) {
		printf("Runtime BPR calibration fail=%d\n",
		       runtime_bpr_ret);
	}
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
	mem_cfg.pool_info[id].blk_cnt = 2 + 8;
	// 8 => TDC out + NUC k + NUC b + NUC out + FFC k + FFC b + FFC out + FFC sum
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;

	// config common pool (main)
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = DBGINFO_BUFSIZE()+VDO_YUV_BUFSIZE(VIDEO_OUTPUT_WIDTH, VIDEO_OUTPUT_HEIGHT, HD_VIDEO_PXLFMT_YUV420);
	mem_cfg.pool_info[id].blk_cnt = 4;
	mem_cfg.pool_info[id].ddr_id = DDR_ID0;

	// TDC common pool (main)
	id++;
	mem_cfg.pool_info[id].type = HD_COMMON_MEM_COMMON_POOL;
	mem_cfg.pool_info[id].blk_size = TDC_blk_size;
	mem_cfg.pool_info[id].blk_cnt = 4; // 需要分配給 DCC_K, LC_K, ADD_output, all_1:  4 個 Block
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

	ret = hd_videoproc_open(0, _out_id, &video_proc_ctrl); //open this for device control
	if (ret != HD_OK)
		return ret;

	if (p_max_dim != NULL ) {
		video_cfg_param.pipe = HD_VIDEOPROC_PIPE_RAWALL;
		video_cfg_param.isp_id = g_thermal_pipeline_id;
		video_cfg_param.ctrl_max.func = VIDEOPROC_ALG_FUNC;
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


	video_ctrl_param.func = VIDEOPROC_ALG_FUNC;
	video_ctrl_param.ref_path_3dnr = HD_VIDEOPROC_0_OUT_4;
	ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_CTRL, &video_ctrl_param);

	video_ll_param.delay_trig_lowlatency = 0;
	ret = hd_videoproc_set(video_proc_ctrl, HD_VIDEOPROC_PARAM_LL_CONFIG, &video_ll_param);

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
	HD_PATH_ID proc_path_diag;

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

	HD_COMMON_MEM_VB_BLK thermal_ffc_k_frame_blk;
	UINTPTR thermal_ffc_k_frame_pa;
	UINTPTR thermal_ffc_k_frame_va;

	HD_COMMON_MEM_VB_BLK thermal_ffc_b_frame_blk;
	UINTPTR thermal_ffc_b_frame_pa;
	UINTPTR thermal_ffc_b_frame_va;

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
	int proc_bound;
	int cap_started;
	int proc_started;
	int proc_3dnr_started;
	int proc_diag_started;
} LIVEVIEW_CONTEXT;

static LIVEVIEW_CONTEXT g_liveview;
static UINT32 g_snapshot_count = 0;
static pthread_mutex_t g_liveview_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_liveview_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_shutter_calibration_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_liveview_shutter_fault = 0;
static int g_liveview_stop_request = 0;
static int g_liveview_fault_stop_worker_active = 0;

static void liveview_stop_pipeline(LIVEVIEW_CONTEXT *ctx);

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
	shutter_calibration_enabled =
		ffc_is_enabled() || bpr_is_runtime_enabled();
	if (active) {
		g_shutter_reschedule_after_manual_calibration = 0;
		g_manual_calibration_active = 1;
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
	pthread_mutex_unlock(&g_shutter_calibration_lock);
}

static int liveview_prepare_shutter_interval(int enabled)
{
	int paused;

	pthread_mutex_lock(&g_shutter_calibration_lock);
	if (g_shutter_reschedule_after_manual_calibration) {
		if (enabled) {
			liveview_schedule_next_shutter_calibration();
		} else {
			g_shutter_next_calibration_ms = 0;
		}
		g_shutter_reschedule_after_manual_calibration = 0;
	}
	paused = g_manual_calibration_active;
	pthread_mutex_unlock(&g_shutter_calibration_lock);

	return paused;
}

static int save_active_raw_frame(const HD_VIDEO_FRAME *p_video_frame,
	const char *file_path)
{
	FILE *fp = NULL;
	UINTPTR blk_pa;
	UINTPTR map_va;
	UINTPTR raw_va;
	UINT32 stride;
	UINT32 row_bytes = RAW_ACTIVE_W * 2;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 y;
	int ret = -1;

	if (p_video_frame == NULL || file_path == NULL || p_video_frame->blk == 0) {
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

	fp = fopen(file_path, "wb");
	if (fp == NULL) {
		printf("snapshot open %s fail\n", file_path);
		goto exit;
	}

	for (y = 0; y < RAW_ACTIVE_H; y++) {
		if (fwrite((void *)(raw_va + y * stride), 1, row_bytes, fp) != row_bytes) {
			printf("snapshot write %s fail at row %u\n", file_path, y);
			goto exit;
		}
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
	printf("snapshot saved: %s (%ux%u, 16-bit container)\n",
	       file_path, RAW_ACTIVE_W, RAW_ACTIVE_H);
	ret = 0;

exit:
	if (fp != NULL) {
		fclose(fp);
	}
	hd_common_mem_munmap((void *)map_va, map_size);
	return ret;
}

static void print_requested_raw_frame_stats(const HD_VIDEO_FRAME *p_video_frame)
{
	UINTPTR blk_pa;
	UINTPTR map_pa;
	UINTPTR map_va;
	UINTPTR raw_va;
	UINT32 data_offset;
	UINT32 stride;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 width;
	UINT32 height;
	UINT32 x;
	UINT32 y;
	UINT16 min_value = 0x3FFF;
	UINT16 max_value = 0;
	UINT16 container_min = 0xFFFF;
	UINT16 container_max = 0;
	UINT64 sum = 0;
	UINT64 container_sum = 0;
	UINT64 low2_count[4] = {0};
	UINT64 pixel_count;

	if (!g_raw_stats_pending) {
		return;
	}
	if (p_video_frame == NULL) {
		printf("RAW diagnostic: frame pointer is NULL\n");
		g_raw_stats_pending = 0;
		return;
	}

	printf("RAW diagnostic frame: blk=0x%llX, pa=0x%llX, "
	       "fmt=0x%08X, dim=%ux%u, loff=%u, ph=%u\n",
	       (unsigned long long)p_video_frame->blk,
	       (unsigned long long)p_video_frame->phy_addr[0],
	       p_video_frame->pxlfmt,
	       p_video_frame->dim.w, p_video_frame->dim.h,
	       p_video_frame->loff[0], p_video_frame->ph[0]);
	if (p_video_frame->phy_addr[0] == 0) {
		printf("RAW diagnostic: physical address is zero\n");
		g_raw_stats_pending = 0;
		return;
	}

	width = p_video_frame->dim.w;
	height = p_video_frame->dim.h;
	stride = p_video_frame->loff[0];
	if (stride < width * sizeof(UINT16)) {
		stride *= sizeof(UINT16);
	}
	plane_height = p_video_frame->ph[0] != 0 ?
		p_video_frame->ph[0] : height;
	if (width == 0 || height == 0 || plane_height < height ||
	    stride < width * sizeof(UINT16)) {
		printf("RAW stats invalid frame: %ux%u, stride=%u, ph=%u\n",
		       width, height, stride, plane_height);
		g_raw_stats_pending = 0;
		return;
	}

	blk_pa = p_video_frame->blk != 0 ?
		hd_common_mem_blk2pa(p_video_frame->blk) : 0;
	if (blk_pa != 0 && p_video_frame->phy_addr[0] >= blk_pa) {
		map_pa = blk_pa;
		data_offset = (UINT32)(p_video_frame->phy_addr[0] - blk_pa);
	} else {
		/* Some VCAP frames do not expose a common-pool block handle. */
		map_pa = p_video_frame->phy_addr[0];
		data_offset = 0;
	}

	map_size = data_offset + stride * plane_height;
	map_va = (UINTPTR)hd_common_mem_mmap(HD_COMMON_MEM_MEM_TYPE_CACHE,
		map_pa, map_size);
	if (map_va == 0) {
		printf("RAW stats mmap error\n");
		g_raw_stats_pending = 0;
		return;
	}

	hd_common_mem_flush_cache((void *)map_va, map_size);
	raw_va = map_va + data_offset;
	for (y = 0; y < height; y++) {
		const UINT16 *row = (const UINT16 *)(raw_va + y * stride);

		for (x = 0; x < width; x++) {
			UINT16 container_value = row[x];
			UINT16 value = container_value & 0x3FFF;

			if (value < min_value) min_value = value;
			if (value > max_value) max_value = value;
			sum += value;
			if (container_value < container_min) container_min = container_value;
			if (container_value > container_max) container_max = container_value;
			container_sum += container_value;
			low2_count[container_value & 0x3U]++;
		}
	}
	pixel_count = (UINT64)width * height;

	printf("RAW frame: fmt=0x%08X, dim=%ux%u, stride=%u, ph=%u\n",
	       p_video_frame->pxlfmt, width, height, stride, plane_height);
	printf("RAW14 stats: min=%u, max=%u, mean=%llu, pixels=%llu\n",
	       min_value, max_value,
	       (unsigned long long)(sum / pixel_count),
	       (unsigned long long)pixel_count);
	printf("RAW16 container: min=%u, max=%u, mean=%llu\n",
	       container_min, container_max,
	       (unsigned long long)(container_sum / pixel_count));
	printf("RAW low2 histogram: 0=%llu, 1=%llu, 2=%llu, 3=%llu\n",
	       (unsigned long long)low2_count[0],
	       (unsigned long long)low2_count[1],
	       (unsigned long long)low2_count[2],
	       (unsigned long long)low2_count[3]);

	hd_common_mem_munmap((void *)map_va, map_size);
	g_raw_stats_pending = 0;
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
	UINT8 min_value = 0xFF;
	UINT8 max_value = 0;
	UINT64 sum = 0;
	UINT64 pixel_count;

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

	hd_common_mem_flush_cache((void *)map_va, map_size);
	y_va = map_va + data_offset;
	for (y = 0; y < height; y++) {
		const UINT8 *row = (const UINT8 *)(y_va + y * stride);

		for (x = 0; x < width; x++) {
			UINT8 value = row[x];

			if (value < min_value) min_value = value;
			if (value > max_value) max_value = value;
			sum += value;
		}
	}
	pixel_count = (UINT64)width * height;
	printf("Y plane stats: min=%u, max=%u, mean=%llu, pixels=%llu\n",
	       min_value, max_value,
	       (unsigned long long)(sum / pixel_count),
	       (unsigned long long)pixel_count);

	hd_common_mem_munmap((void *)map_va, map_size);
	g_yuv_stats_pending = 0;
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

		dir = opendir(SNAPSHOT_DIR);
		if (dir == NULL) {
			printf("snapshot scan %s fail\n", SNAPSHOT_DIR);
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

static void save_pipeline_snapshot(HD_VIDEO_FRAME *p_input_frame,
	HD_VIDEO_FRAME *p_final_frame,
	int final_is_input)
{
	char file_path[160];
	UINT32 snapshot_id;

	if (snapshot_get_next_id(&snapshot_id) != 0) {
		return;
	}

	if (final_is_input) {
		snprintf(file_path, sizeof(file_path),
			 SNAPSHOT_DIR "/snapshot_%04u_raw16_%ux%u.raw",
			 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
		save_active_raw_frame(p_input_frame, file_path);
		return;
	}

	snprintf(file_path, sizeof(file_path),
		 SNAPSHOT_DIR "/snapshot_%04u_input_raw16_%ux%u.raw",
		 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
	save_active_raw_frame(p_input_frame, file_path);

	snprintf(file_path, sizeof(file_path),
		 SNAPSHOT_DIR "/snapshot_%04u_corrected_raw16_%ux%u.raw",
		 snapshot_id, RAW_ACTIVE_W, RAW_ACTIVE_H);
	save_active_raw_frame(p_final_frame, file_path);
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

static HD_RESULT init_thermal_frame(VIDEO_LIVEVIEW *p_stream)
{
	UINT32 i;
	UINT32 blk_size = VDO_RAW_BUFSIZE(RAW_SIZE_W, RAW_SIZE_H, CAP_OUT_FMT);

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

	//temporarily set to K=1.0 in Q8 format
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
			(NUC_GAIN_ONE << 16) | NUC_GAIN_ONE;
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

#undef RELEASE_THERMAL_BUFFER

	p_stream->thermal_k_frame_pa = 0;
	p_stream->thermal_tdc_out_pa = 0;
	p_stream->thermal_b_frame_pa = 0;
	p_stream->thermal_nuc_out_pa = 0;
	p_stream->thermal_ffc_k_frame_pa = 0;
	p_stream->thermal_ffc_b_frame_pa = 0;
	p_stream->thermal_ffc_out_pa = 0;
	p_stream->thermal_ffc_sum_pa = 0;

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
	if ((ret = hd_videoproc_open(HD_VIDEOPROC_0_IN_0, HD_VIDEOPROC_0_OUT_4, &p_stream->proc_path_3dnr)) != HD_OK)
		return ret;
	if ((ret = hd_videoproc_open(HD_VIDEOPROC_0_IN_0, HD_VIDEOPROC_0_OUT_1, &p_stream->proc_path_diag)) != HD_OK)
		return ret;
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
	CLOSE_PATH(p_stream->proc_path_diag, hd_videoproc_close, "videoproc diagnostic");
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

	if (nuc_is_enabled() && now_ms >= *nuc_next_update_ms) {
		nuc_update_sensor_temp_table();
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
}

static int liveview_run_ffc_calibration(
	VIDEO_LIVEVIEW *p_stream0,
	UINT64 *nuc_next_update_ms,
	UINT64 *tdc_next_update_ms)
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
			if (!ffc_is_enabled() && !bpr_is_runtime_enabled()) {
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
			process_ret = nuc_apply(calibration_frame,
						&nuc_video_frame,
						p_stream0->thermal_k_frame_pa,
						p_stream0->thermal_b_frame_pa,
						p_stream0->thermal_nuc_out_pa,
						p_stream0->thermal_nuc_out_blk,
						RAW_SIZE_W,
						RAW_SIZE_H);
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
			sum_pa = p_stream0->thermal_ffc_sum_pa;
			out_pa = p_stream0->thermal_ffc_out_pa;
		}
		if (frame == FFC_AVERAGE_FRAME_COUNT - 1U) {
			shift = FFC_AVERAGE_SHIFT;
		}

		if (process_ret == HD_OK) {
			process_ret = ffc_accumulate_frame(
				calibration_frame->phy_addr[0],
				sum_pa, out_pa,
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
	}

	return final_release_failed ? -1 : 0;
}

static int liveview_run_shutter_calibration_cycle(
	VIDEO_LIVEVIEW *p_stream0,
	UINT64 *nuc_next_update_ms,
	UINT64 *tdc_next_update_ms)
{
	int calibration_ok = 1;
	int shutter_result = 0;

	pthread_mutex_lock(&g_shutter_calibration_lock);
	if (g_manual_calibration_active) {
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		return SHUTTER_CALIBRATION_PAUSED;
	}

	if (gpio_shutter_close() != 0) {
		printf("Shutter calibration close shutter fail\n");
		pthread_mutex_unlock(&g_shutter_calibration_lock);
		return -1;
	}

	liveview_sleep_ms(g_shutter_close_wait_ms);

	if (ffc_is_enabled()) {
		if (liveview_run_ffc_calibration(p_stream0,
						 nuc_next_update_ms,
						 tdc_next_update_ms) != 0) {
			calibration_ok = 0;
		}
	}

	if (bpr_is_runtime_enabled()) {
		if (calibration_ok) {
			liveview_run_runtime_bpr_while_shutter_closed();
		} else {
			printf("Skip Runtime BPR because FFC calibration failed\n");
		}
	}

	if (gpio_shutter_open() != 0) {
		printf("Shutter calibration open shutter fail\n");
		shutter_result = -1;
	}

	pthread_mutex_unlock(&g_shutter_calibration_lock);
	return shutter_result;
}

static void liveview_process_live_frame(VIDEO_LIVEVIEW *p_stream0)
{
	HD_VIDEO_FRAME vcap_video_frame = {0};
	HD_VIDEO_FRAME tdc_video_frame = {0};
	HD_VIDEO_FRAME nuc_video_frame = {0};
	HD_VIDEO_FRAME ffc_video_frame = {0};
	HD_VIDEO_FRAME *current_frame = &vcap_video_frame;
	int current_frame_is_vcap = 1;
	int raw_copy_handled = 0;
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
	print_requested_raw_frame_stats(&vcap_video_frame);

	if (g_raw_copy_pending) {
		g_raw_copy_result =
			copy_videoframe_raw(&vcap_video_frame,
					    g_raw_copy_dst_va,
					    g_raw_copy_size);
		g_raw_copy_pending = 0;
		g_raw_copy_done = 1;
		raw_copy_handled = 1;
	}

	/* Pipeline: VCAP -> optional TDC -> optional NUC -> optional FFC -> VideoProc. */
	if (tdc_is_enabled()) {
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

	if (nuc_is_enabled()) {
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
	}

	if (ffc_is_enabled() && ffc_table_is_ready()) {
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

	if (p_stream0->cap_snap) {
		p_stream0->cap_snap = 0;
		save_pipeline_snapshot(&vcap_video_frame, current_frame,
				       current_frame_is_vcap);
	}

	/* A processed frame no longer depends on the held VCAP buffer. */
	if (!current_frame_is_vcap) {
		ret = hd_videocap_release_out_buf(
			p_stream0->cap_path, &vcap_video_frame);
		if (ret != HD_OK) {
			printf("cap_release error=%d !!\r\n\r\n", ret);
			return;
		}
	}

	if (!g_proc_input_format_logged) {
		printf("VideoProc input format: frame=0x%08X, configured=0x%08X\n",
		       current_frame->pxlfmt, CAP_OUT_FMT);
		g_proc_input_format_logged = 1;
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
		return;
	}

	/* Do not delay the next calibration RAW request with runtime skipping. */
	if (!raw_copy_handled && g_runtime_skip_frames > 0) {
		skip_vcap_frame(p_stream0, g_runtime_skip_frames);
	}
	return;

release_vcap_frame:
	ret = hd_videocap_release_out_buf(
		p_stream0->cap_path, &vcap_video_frame);
	if (ret != HD_OK) {
		printf("cap_release error=%d !!\r\n\r\n", ret);
	}
}

static void *raw_pipeline_thread(void *arg)
{
	VIDEO_LIVEVIEW* p_stream0 = (VIDEO_LIVEVIEW *)arg;
	UINT64 tdc_next_update_ms = 0;
	UINT64 nuc_sensor_temp_next_update_ms = 0;
	int shutter_calibration_enabled;
	int shutter_calibration_result;
	int shutter_calibration_paused;

	//------ wait flow_start ------
	while (p_stream0->cap_enter == 0 && p_stream0->cap_exit == 0) usleep(100);
	if (p_stream0->cap_exit) {
		return 0;
	}

	// Skip unstable frames once when the liveview pipeline starts.
	printf("startup skip %u frame ++\r\n", g_startup_skip_frames);
	skip_vcap_frame(p_stream0, g_startup_skip_frames);
	printf("startup skip %u frame --\r\n", g_startup_skip_frames);

	shutter_calibration_enabled =
		ffc_is_enabled() || bpr_is_runtime_enabled();
	g_shutter_next_calibration_ms = 0;
	g_shutter_reschedule_after_manual_calibration = 0;
	if (shutter_calibration_enabled) {
		liveview_schedule_next_shutter_calibration();
	}

	while (p_stream0->cap_exit == 0) {
		shutter_calibration_paused =
			liveview_prepare_shutter_interval(
				shutter_calibration_enabled);

		liveview_update_temperature_tables_if_needed(
			&nuc_sensor_temp_next_update_ms,
			&tdc_next_update_ms);

		if (shutter_calibration_enabled &&
		    !shutter_calibration_paused &&
		    liveview_get_time_ms() >=
			g_shutter_next_calibration_ms) {
			shutter_calibration_result =
				liveview_run_shutter_calibration_cycle(
					p_stream0,
					&nuc_sensor_temp_next_update_ms,
					&tdc_next_update_ms);
			if (shutter_calibration_result < 0) {
				g_shutter_next_calibration_ms = 0;
				break;
			}

			if (shutter_calibration_result == 0) {
				liveview_schedule_next_shutter_calibration();
			}
			usleep(100);
			continue;
		}

		liveview_process_live_frame(p_stream0);
		usleep(100);
	}

	return 0;
}

static void *yuv_statistics_thread(void *arg)
{
	VIDEO_LIVEVIEW *p_stream0 = (VIDEO_LIVEVIEW *)arg;
	HD_VIDEO_FRAME video_frame = {0};
	HD_RESULT ret;

	while (p_stream0->proc_enter == 0 && p_stream0->proc_exit == 0) {
		usleep(100);
	}
	while (p_stream0->proc_exit == 0) {
		ret = hd_videoproc_pull_out_buf(
			p_stream0->proc_path_diag, &video_frame, 100);
		if (ret != HD_OK) {
			if (ret != HD_ERR_UNDERRUN && ret != HD_ERR_TIMEDOUT) {
				printf("diagnostic proc_pull error=%d !!\r\n\r\n", ret);
			}
			continue;
		}

		print_requested_yuv_frame_stats(&video_frame);
		ret = hd_videoproc_release_out_buf(
			p_stream0->proc_path_diag, &video_frame);
		if (ret != HD_OK) {
			printf("diagnostic proc_release error=%d !!\r\n\r\n", ret);
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

	cfg_info.id = g_thermal_pipeline_id;
	strncpy(cfg_info.path, "/mnt/app/isp/isp_rtd6122c_0.cfg", CFG_NAME_LENGTH);
	ret = vendor_isp_set_iq(IQT_ITEM_RLD_CONFIG, &cfg_info);
	if (ret != HD_OK) {
		printf("ISP IQ reload failed=%d, path=%s\n", ret, cfg_info.path);
		ret = HD_OK;
	} else {
		printf("ISP IQ reload ok: %s\n", cfg_info.path);
	}

	ret = gamma_init_from_isp();
	if (ret != HD_OK) {
		printf("init Gamma from ISP failed=%d, keep ISP cfg Gamma\n", ret);
		ret = HD_OK;
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
	ret = gamma_apply();
	if (ret != HD_OK) {
		printf("apply custom Gamma failed=%d, keep available Gamma settings\n",
		       ret);
		ret = HD_OK;
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
		HD_VIDEO_PXLFMT_YUV420, 0);
	if (ret != HD_OK) {
		printf("set proc fail=%d\n", ret);
		goto exit;
	}

	ret = set_proc_param(stream[0].proc_path_3dnr, &main_dim,
		HD_VIDEO_PXLFMT_YUV420, 0);
	if (ret != HD_OK) {
		printf("set proc fail = %d \n", ret);
		goto exit;
	}
	/* OUT_1 is independent from video output OUT_0 and 3DNR reference OUT_4. */
	ret = set_proc_param(stream[0].proc_path_diag, &main_dim,
		HD_VIDEO_PXLFMT_YUV420, 1);
	if (ret != HD_OK) {
		printf("set diagnostic proc fail=%d\n", ret);
		goto exit;
	}

	// create raw_pipeline_thread (VCAP -> TDC -> NUC -> FFC -> VideoProc)
	ret = pthread_create(&stream[0].cap_thread_id, NULL, raw_pipeline_thread, (void *)stream);
	if (ret != 0) {
		printf("create raw_pipeline_thread failed");
		goto exit;
	}
	ctx->cap_thread_created = 1;
	ret = pthread_create(&stream[0].proc_thread_id, NULL,
		yuv_statistics_thread, (void *)stream);
	if (ret != 0) {
		printf("create yuv_statistics_thread failed\n");
		goto exit;
	}
	ctx->proc_thread_created = 1;

	ret = hd_videoproc_bind(HD_VIDEOPROC_0_OUT_0,
		HD_VIDEOENC_0_IN_0);
	if (ret != HD_OK) {
		printf("bind videoproc to %s encoder failed=%d\n",
			video_output_mode_name(g_output_mode), ret);
		goto exit;
	}
	ctx->proc_bound = 1;

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

	ret = hd_videoproc_start(stream[0].proc_path_3dnr);
	if (ret != HD_OK) {
		printf("videoproc 3DNR start failed=%d\n", ret);
		goto exit;
	}
	ctx->proc_3dnr_started = 1;
	ret = hd_videoproc_start(stream[0].proc_path_diag);
	if (ret != HD_OK) {
		printf("diagnostic videoproc start failed=%d\n", ret);
		goto exit;
	}
	ctx->proc_diag_started = 1;
	// wait thermal stable
	sleep(2);
	ret = video_output_start(&stream[0].video_output);
	if (ret != HD_OK) {
		printf("%s output start failed=%d\n",
			video_output_mode_name(g_output_mode), ret);
		goto exit;
	}

	// start raw_pipeline_thread
	printf("RAW diagnostic build: metadata+stats v2 enabled\n");
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

	/* Wake threads that may still be waiting for the pipeline to start. */
	stream[0].cap_exit = 1;
	stream[0].proc_exit = 1;

	if (ctx->cap_thread_created) {
		pthread_join(stream[0].cap_thread_id, NULL);
		ctx->cap_thread_created = 0;
		stream[0].cap_thread_id = 0;
	}
	if (ctx->proc_thread_created) {
		pthread_join(stream[0].proc_thread_id, NULL);
		ctx->proc_thread_created = 0;
		stream[0].proc_thread_id = 0;
	}


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
	if (ctx->proc_diag_started) {
		ret = hd_videoproc_stop(stream[0].proc_path_diag);
		if (ret != HD_OK) printf("diagnostic videoproc stop failed=%d\n", ret);
		ctx->proc_diag_started = 0;
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
	bpr_bind_calibration_state_callback(NULL);
	nuc_bind_runtime(0, 0, 0, NULL);
	tdc_bind_capture_callback(NULL);
	ffc_bind_runtime(0, 0);
	g_raw_copy_pending = 0;
	g_raw_copy_done = 0;
	g_yuv_stats_pending = 0;
	g_proc_input_format_logged = 0;
	g_shutter_next_calibration_ms = 0;
	g_manual_calibration_active = 0;
	g_shutter_reschedule_after_manual_calibration = 0;

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
	ret = liveview_start_pipeline(&g_liveview);

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
	return ret;
}

int liveview_is_running(void)
{
	return g_liveview.running;
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

	g_liveview.stream[0].cap_snap = 1;
	return 0;
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
		printf(" 5. Show current RAW statistics\n");
		printf(" 6. Show current YUV statistics\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Settings: CFG Setting -> Liveview setting\n");

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
			ret = liveview_request_raw_statistics();
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
	gpio_shutter_set_failure_callback(NULL);
	g_liveview.initialized = 0;

exit:
	pthread_mutex_unlock(&g_liveview_lifecycle_lock);
}
