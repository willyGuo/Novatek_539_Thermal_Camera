
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#include "dpc_lib.h"
#include "vendor_isp.h"

#include "kwrap/file.h"
#include <kwrap/util.h>
#include "hdal.h"
#include "hd_videocapture.h"
#include "bpr.h"
#include "../../Config/hardware_config.h"

#define VENDOR_DPC_ISP_CFG_PATH "/mnt/app/isp/isp_rtd6122c_0.cfg"
#define VENDOR_DPC_BIN_PATH "/mnt/app/isp/dpc_table.bin"
#define BPR_MAX_DP_COUNT 4095U

unsigned int dpc_get_sensor_info(int sensor_id, ISPT_SENSOR_MODE_INFO *p_sensor_mode_info);
void convert_16bit_to_8bit(const UINT16 *src_addr, UINT8 *dest_addr, unsigned int width, unsigned int height);
unsigned int dpc_get_raw(int sensor_id, ISPT_RAW_INFO *final_raw_info, UINT32 *raw_buf);

// DPC 壞點校正範例：
// 從 ISP 抓 RAW，呼叫 vendor 壞點演算法產生 DPC table，
// 再透過重新載入 ISP cfg 或開關 DPC 來套用補償。

/*
 * 從終端機讀取一個 unsigned int。
 *
 * 用途：
 *   這支程式是互動式選單工具，很多地方都需要使用者輸入數字，
 *   例如 sensor id、選單項目、threshold、DPC on/off。
 *
 * 行為：
 *   - 如果使用者輸入的是合法整數，就回傳該數字。
 *   - 如果輸入不是數字，會清掉錯誤狀態並要求重新輸入。
 */
static unsigned int get_choose_uint(void)
{
	char str_buf[32];
	unsigned int val, error;
	do {
		error = scanf(" %d", &val);
		if (error != 1) {
			printf("Invalid option. Try again.\n");
			clearerr(stdin);
			fgets(str_buf, sizeof(str_buf), stdin);
			printf(">> ");
		}
	} while(error != 1);

	return val;
}

static int wait_enter_before_raw(const char *stage_name)
{
	int key;

	do {
		key = getchar();
	} while (key != '\n' && key != '\r' && key != EOF);

	printf("%s: Press Enter key after exposure is stable...\n", stage_name);
	key = getchar();
	if (key == '\n' || key == '\r') {
		printf("Enter key pressed, continue...\n");
		return 0;
	}

	printf("Invalid key pressed, break...\n");
	return -1;
}

static void print_sensor_crop_setting(const ISPT_SENSOR_MODE_INFO *sensor_mode_info)
{
	unsigned int crp_x;
	unsigned int crp_y;

	crp_x = (sensor_mode_info->info.act_size[0].w - sensor_mode_info->info.crp_size.w) / 2;
	crp_y = (sensor_mode_info->info.act_size[0].h - sensor_mode_info->info.crp_size.h) / 2;

	printf("Sensor active: x=%u y=%u w=%u h=%u\r\n",
		sensor_mode_info->info.act_size[0].x,
		sensor_mode_info->info.act_size[0].y,
		sensor_mode_info->info.act_size[0].w,
		sensor_mode_info->info.act_size[0].h);
	printf("Sensor crop  : w=%u h=%u\r\n",
		sensor_mode_info->info.crp_size.w,
		sensor_mode_info->info.crp_size.h);
	printf("DPC crop offset: x=%u y=%u\r\n", crp_x, crp_y);
}


static IQT_DPC_PARAM dpc_param;
static IQT_SHADING_PARAM shading_par;
static unsigned int g_bpr_threshold = 15;
static int g_bpr_enabled = 1;
static unsigned int g_runtime_bpr_threshold = 15;
static int g_runtime_bpr_enabled;
static BPR_CALIBRATION_STATE_CB g_bpr_calibration_state;

void bpr_bind_calibration_state_callback(
	BPR_CALIBRATION_STATE_CB calibration_state)
{
	g_bpr_calibration_state = calibration_state;
}

void bpr_set_threshold(unsigned int threshold)
{
	g_bpr_threshold = threshold;
	printf("BPR threshold=%u%%\n", g_bpr_threshold);
}

unsigned int bpr_get_threshold(void)
{
	return g_bpr_threshold;
}

int bpr_set_enabled(int enable)
{
	int ret;

	g_bpr_enabled = enable ? 1 : 0;

	dpc_param.id = g_thermal_pipeline_id;
	ret = vendor_isp_get_iq(IQT_ITEM_DPC_PARAM, &dpc_param);
	if (ret < 0) {
		printf("GET IQT_ITEM_DPC_PARAM fail!\n");
		return ret;
	}

	dpc_param.dpc.enable = g_bpr_enabled;
	ret = vendor_isp_set_iq(IQT_ITEM_DPC_PARAM, &dpc_param);
	if (ret < 0) {
		printf("SET IQT_ITEM_DPC_PARAM fail!\n");
		return ret;
	}

	printf("BPR %s\n", g_bpr_enabled ? "on" : "off");
	return 0;
}

int bpr_is_enabled(void)
{
	return g_bpr_enabled;
}

void bpr_set_runtime_enabled(int enable)
{
	g_runtime_bpr_enabled = enable ? 1 : 0;
	printf("Runtime BPR %s\n",
	       g_runtime_bpr_enabled ? "on" : "off");
}

int bpr_is_runtime_enabled(void)
{
	return g_runtime_bpr_enabled;
}

void bpr_set_runtime_threshold(unsigned int threshold)
{
	g_runtime_bpr_threshold = threshold;
	printf("Runtime BPR threshold=%u%%\n",
	       g_runtime_bpr_threshold);
}

unsigned int bpr_get_runtime_threshold(void)
{
	return g_runtime_bpr_threshold;
}

static int dpc_load_cfg(IQT_CFG_INFO *cfg_info)
{
	int ret;
	int dpc_restore_ret;
	int gamma_restore_ret;
	int enabled = g_bpr_enabled;
	IQT_GAMMA_PARAM saved_gamma = {0};

	saved_gamma.id = cfg_info->id;
	ret = vendor_isp_get_iq(IQT_ITEM_GAMMA_PARAM, &saved_gamma);
	if (ret < 0) {
		printf("get Gamma before DPC cfg reload fail!\n");
		return ret;
	}

	strncpy(cfg_info->path, VENDOR_DPC_ISP_CFG_PATH, CFG_NAME_LENGTH);
	cfg_info->path[CFG_NAME_LENGTH - 1] = '\0';

	ret = vendor_isp_set_iq(IQT_ITEM_RLD_CONFIG, cfg_info);
	if (ret < 0) {
		printf("SET IQT_ITEM_RLD_CONFIG fail!\n");
		return ret;
	}

	dpc_restore_ret = bpr_set_enabled(enabled);
	if (dpc_restore_ret < 0) {
		printf("restore DPC enable fail!\n");
	}

	gamma_restore_ret = vendor_isp_set_iq(
		IQT_ITEM_GAMMA_PARAM, &saved_gamma);
	if (gamma_restore_ret < 0) {
		printf("restore Gamma after DPC cfg reload fail!\n");
	}

	if (dpc_restore_ret < 0) {
		return dpc_restore_ret;
	}
	if (gamma_restore_ret < 0) {
		return gamma_restore_ret;
	}

	printf("DPC cfg reload done: %s\r\n", VENDOR_DPC_ISP_CFG_PATH);
	return 0;
}

static int dpc_save_bin(UINT32 *dp_buf)
{
	FILE *fp;
	size_t write_size;

	fp = fopen(VENDOR_DPC_BIN_PATH, "wb");
	if (fp == NULL) {
		printf("open %s fail\r\n", VENDOR_DPC_BIN_PATH);
		return -1;
	}

	write_size = fwrite(dp_buf, 1, 4096 * sizeof(UINT32), fp);
	fflush(fp);
	fclose(fp);
	if (write_size != 4096 * sizeof(UINT32)) {
		printf("write %s fail, size=%u\r\n", VENDOR_DPC_BIN_PATH, (unsigned int)write_size);
		return -1;
	}

	printf("save DPC table: %s, dp_buf:%lx\r\n", VENDOR_DPC_BIN_PATH, (ULONG)dp_buf);
	return 0;
}

static int dpc_alloc_buf(unsigned int raw_size, UINT32 max_cnt,
	char **raw_buf, char **tmp_buf, UINT32 **dp_buf)
{
	*raw_buf = (char *)malloc(sizeof(char) * raw_size);
	*tmp_buf = (char *)malloc(sizeof(char) * raw_size * 2);
	*dp_buf = (UINT32 *)malloc((max_cnt + 1) * sizeof(UINT32));
	if (*raw_buf == NULL || *tmp_buf == NULL || *dp_buf == NULL) {
		printf("fail to allocate memory for DPC buffer!\n");
		if (*raw_buf != NULL) {
			free(*raw_buf);
			*raw_buf = NULL;
		}
		if (*tmp_buf != NULL) {
			free(*tmp_buf);
			*tmp_buf = NULL;
		}
		if (*dp_buf != NULL) {
			free(*dp_buf);
			*dp_buf = NULL;
		}
		return -1;
	}

	memset(*raw_buf, 0, sizeof(char) * raw_size);
	memset(*tmp_buf, 0, sizeof(char) * raw_size * 2);
	memset(*dp_buf, 0, (max_cnt + 1) * sizeof(UINT32));

	return 0;
}

static void dpc_free_buf(char **raw_buf, char **tmp_buf, UINT32 **dp_buf)
{
	if (*raw_buf != NULL) {
		free(*raw_buf);
		*raw_buf = NULL;
	}
	if (*tmp_buf != NULL) {
		free(*tmp_buf);
		*tmp_buf = NULL;
	}
	if (*dp_buf != NULL) {
		free(*dp_buf);
		*dp_buf = NULL;
	}
}

static int dpc_run_raw(const char *step_name, unsigned int sensor_id,
	ISPT_RAW_INFO *raw_info, const ISPT_SENSOR_MODE_INFO *sensor_mode_info,
	char *raw_buf, char *tmp_buf, UINT32 *dp_buf, unsigned int raw_size,
	unsigned int dpc_th, UINT32 ori_dp_cnt, UINT32 max_cnt, UINT32 chg_fmt,
	CAL_ALG_DP_RST *dp_rst)
{
	CAL_ALG_DP_PARAM dp_param;
	unsigned int avg = 0;
	unsigned int j;

	if (max_cnt == 0 || ori_dp_cnt > max_cnt) {
		printf("%s DPC input count overflow: count=%u, max=%u; abort\r\n",
		       step_name, ori_dp_cnt, max_cnt);
		return -1;
	}

	raw_info->id = sensor_id;
	printf("%s: capture RAW.\r\n", step_name);
	if (dpc_get_raw(sensor_id, raw_info, (UINT32 *)tmp_buf) != 0) {
		return -1;
	}

	if (raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW8) {
		memcpy(raw_buf, tmp_buf, raw_size);
		raw_info->raw_info.addr = (ULONG)raw_buf;
		printf("VDO_PXLFMT_RAW8\n");
	} else if (raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW16) {
		convert_16bit_to_8bit((const UINT16 *)tmp_buf, (UINT8 *)raw_buf,
			raw_info->raw_info.pw, raw_info->raw_info.ph);
		raw_info->raw_info.addr = (ULONG)raw_buf;
		printf("VDO_PXLFMT_RAW16 -> RAW8\n");
	} else {
		printf("error raw format\r\n");
		return -1;
	}

	for (j = 0; j < raw_size; j++) {
		avg += (unsigned char)raw_buf[j];
	}
	avg /= raw_size;
	printf("average lum = %d\r\n", avg);

	memset(&dp_param, 0, sizeof(dp_param));
	dp_param.raw_info.pw = raw_info->raw_info.pw;
	dp_param.raw_info.ph = raw_info->raw_info.ph;
	dp_param.raw_info.loff = raw_info->raw_info.pw;
	dp_param.raw_info.addr = (ULONG)raw_info->raw_info.addr;
	dp_param.sie_act_str.x = sensor_mode_info->info.act_size[0].x;
	dp_param.sie_act_str.y = sensor_mode_info->info.act_size[0].y;
	dp_param.sie_crp_str.x = (sensor_mode_info->info.act_size[0].w - sensor_mode_info->info.crp_size.w) / 2;
	dp_param.sie_crp_str.y = (sensor_mode_info->info.act_size[0].h - sensor_mode_info->info.crp_size.h) / 2;
	dp_param.dp_pool_addr = (ULONG)&dp_buf[0];
	dp_param.setting.threshold = dpc_th;
	dp_param.ori_dp_cnt = ori_dp_cnt;
	dp_param.max_dp_cnt = max_cnt;
	dp_param.b_chg_dp_format = chg_fmt;

	cal_dp_process(&dp_param, dp_rst);
	printf("cnt=%d,data_length=%d \r\n", dp_rst->pixel_cnt, dp_rst->data_length);
	if (dp_rst->pixel_cnt > max_cnt) {
		printf("%s DPC result overflow: count=%u, max=%u; abort\r\n",
		       step_name, dp_rst->pixel_cnt, max_cnt);
		return -1;
	}

	return 0;
}

int bpr_run_runtime_calibration(void)
{
	ISPT_RAW_INFO raw_info = {0};
	ISPT_SENSOR_MODE_INFO sensor_mode_info;
	IQT_CFG_INFO cfg_info = {0};
	CAL_ALG_DP_RST dp_rst;
	char *raw_buf = NULL;
	char *tmp_buf = NULL;
	UINT32 *dp_buf = NULL;
	unsigned int raw_size;
	int ret = -1;

	if (!g_runtime_bpr_enabled) {
		return 0;
	}

	raw_info.id = g_thermal_pipeline_id;
	if (dpc_get_sensor_info(g_thermal_pipeline_id, &sensor_mode_info) != 0) {
		printf("Runtime BPR get sensor info fail\n");
		return -1;
	}
	raw_size = sensor_mode_info.info.crp_size.w *
		sensor_mode_info.info.crp_size.h;
	print_sensor_crop_setting(&sensor_mode_info);
	printf("Runtime BPR start: threshold=%u%%, raw_size=%u\r\n",
	       g_runtime_bpr_threshold, raw_size);

	if (dpc_alloc_buf(raw_size, BPR_MAX_DP_COUNT,
			  &raw_buf, &tmp_buf, &dp_buf) != 0) {
		goto exit;
	}

	memset(&dp_rst, 0, sizeof(dp_rst));
	if (dpc_run_raw("Runtime BPR Shutter Close Pixel",
			g_thermal_pipeline_id, &raw_info, &sensor_mode_info,
			raw_buf, tmp_buf, dp_buf, raw_size,
			g_runtime_bpr_threshold, 0, BPR_MAX_DP_COUNT,
			DP_PARAM_CHGFMT_AUTO, &dp_rst) != 0) {
		goto exit;
	}

	if (dpc_save_bin(dp_buf) != 0) {
		goto exit;
	}

	cfg_info.id = g_thermal_pipeline_id;
	if (dpc_load_cfg(&cfg_info) != 0) {
		goto exit;
	}

	printf("Runtime BPR calibration done\n");
	ret = 0;

exit:
	dpc_free_buf(&raw_buf, &tmp_buf, &dp_buf);
	return ret;
}

static int dpc_run_bright_dark(unsigned int sensor_id, unsigned int dpc_th, UINT32 max_cnt)
{
	ISPT_RAW_INFO raw_info = {0};
	ISPT_SENSOR_MODE_INFO sensor_mode_info;
	CAL_ALG_DP_RST dp_rst;
	char *raw_buf = NULL;
	char *tmp_buf = NULL;
	UINT32 *dp_buf = NULL;
	BPR_CALIBRATION_STATE_CB calibration_state = 0;
	unsigned int raw_size;
	int ret = -1;

	raw_info.id = sensor_id;

	dpc_get_sensor_info(sensor_id, &sensor_mode_info);
	raw_size = sensor_mode_info.info.crp_size.w * sensor_mode_info.info.crp_size.h;
	print_sensor_crop_setting(&sensor_mode_info);
	printf("DPC raw_size = %u\r\n", raw_size);

	if (dpc_alloc_buf(raw_size, max_cnt, &raw_buf, &tmp_buf, &dp_buf) != 0) {
		goto exit;
	}

	calibration_state = g_bpr_calibration_state;
	if (calibration_state != 0) {
		calibration_state("BPR", 1);
	}

	printf("BPR cfg threshold = %u%%\r\n\r\n", dpc_th);

	printf("\n Bright Pixel: blackbody calibration.\r\n\n");
	if (wait_enter_before_raw("Bright Pixel") != 0) {
		goto exit;
	}

	memset(&dp_rst, 0, sizeof(dp_rst));
	if (dpc_run_raw("Bright Pixel", sensor_id, &raw_info, &sensor_mode_info,
		raw_buf, tmp_buf, dp_buf, raw_size, dpc_th, 0, max_cnt,
		DP_PARAM_CHGFMT_SKIP, &dp_rst) != 0) {
		goto exit;
	}

	printf("\n Dark Pixel: blackbody calibration.\r\n");
	printf("Dark Pixel: reuse dpc threshold = %u%%\r\n", dpc_th);
	if (wait_enter_before_raw("Dark Pixel") != 0) {
		goto exit;
	}

	if (dpc_run_raw("Dark Pixel", sensor_id, &raw_info, &sensor_mode_info,
		raw_buf, tmp_buf, dp_buf, raw_size, dpc_th, dp_rst.pixel_cnt, max_cnt,
		DP_PARAM_CHGFMT_AUTO, &dp_rst) != 0) {
		goto exit;
	}

	ret = dpc_save_bin(dp_buf);
	if (ret < 0) {
		printf("save dpc bin fail=%d\n", ret);
		goto exit;
	}

	usleep(1000000);

exit:
	dpc_free_buf(&raw_buf, &tmp_buf, &dp_buf);
	if (calibration_state != 0) {
		calibration_state("BPR", 0);
	}
	return ret;
}

/*
 * 暫時關閉 ECS / shading。
 *
 * 用途：
 *   DPC 校正要看比較接近 sensor 原始狀態的 RAW。
 *   如果 shading/ECS 先把畫面補償過，可能會影響壞點判斷。
 *
 * 輸入：
 *   sensor_id：要操作的 sensor。
 *   ecs_enable：用來保存原本 ECS enable 狀態，之後 restore_ecs() 會用到。
 *
 * 回傳：
 *   0 或正值代表成功，-1 代表 vendor ISP API 失敗。
 */
static int bypass_ecs(int sensor_id, UINT32 *ecs_enable)
{
	int ret = 0;

	shading_par.id = sensor_id;

	ret = vendor_isp_get_iq(IQT_ITEM_SHADING_PARAM, &shading_par);
	if (ret < 0) {
		printf("GET IQT_ITEM_SHADING_PARAM fail! \n");
		return -1;
	}

	*ecs_enable = shading_par.shading.ecs_enable;
	shading_par.shading.ecs_enable = DISABLE;

	ret = vendor_isp_set_iq(IQT_ITEM_SHADING_PARAM, &shading_par);
	if (ret < 0) {
		printf("SET IQT_ITEM_SHADING_PARAM fail! \n");
		return -1;
	}
	return ret;
}

/*
 * 還原 ECS / shading 狀態。
 *
 * 用途：
 *   dpc_get_raw() 抓完 RAW 後，要把 bypass_ecs() 暫時關掉的設定恢復，
 *   避免影響後續正常 ISP 影像流程。
 *
 * 輸入：
 *   sensor_id：要操作的 sensor。
 *   ecs_enable：bypass_ecs() 保存下來的原始狀態。
 */
static int restore_ecs(int sensor_id, UINT32 ecs_enable)
{
	int ret = 0;

	shading_par.id = sensor_id;

	ret = vendor_isp_get_iq(IQT_ITEM_SHADING_PARAM, &shading_par);
	if (ret < 0) {
		printf("GET IQT_ITEM_SHADING_PARAM fail! \n");
		return -1;
	}

	shading_par.shading.ecs_enable = ecs_enable;

	ret = vendor_isp_set_iq(IQT_ITEM_SHADING_PARAM, &shading_par);
	if (ret < 0) {
		printf("SET IQT_ITEM_SHADING_PARAM fail! \n");
		return -1;
	}
	return ret;
}

/*
 * 取得 sensor mode 資訊。
 *
 * 用途：
 *   DPC 演算法需要知道 RAW 寬高、active 區、crop 區等資訊，
 *   才能正確計算壞點座標並輸出 DPC table。
 *
 * 輸入：
 *   sensor_id：要查詢的 sensor。
 *   p_sensor_mode_info：由呼叫者提供，用來接收 ISP 回傳的 sensor mode。
 */
unsigned int dpc_get_sensor_info(int sensor_id,  ISPT_SENSOR_MODE_INFO *p_sensor_mode_info)
{
	HD_RESULT ret = 0;

	p_sensor_mode_info->id = sensor_id;
	ret = vendor_isp_get_common(ISPT_ITEM_SENSOR_MODE_INFO, p_sensor_mode_info);
	
    if (ret < 0) {				
            printf("GET_SENSOR_MODE_INFO fail! \n");
            return -1;
        }
	return ret;
}

static unsigned int g_dbg_print = 0;
static char* dbg_file_path = NULL;


/*
 * 將 RAW16 轉成 RAW8。
 *
 * 用途：
 *   這個範例的 vendor DPC 演算法使用 8-bit RAW。
 *   如果 ISP 抓出來是 RAW16，就先轉成 8-bit 再丟給 cal_dp_process()。
 *
 * 格式：
 *   每個 RAW16 pixel 佔 2 bytes，取出高 8 bits（bit 15..8）作為
 *   DPC calibration 使用的 RAW8。
 */
void convert_16bit_to_8bit(const UINT16 *src_addr, UINT8 *dest_addr,
	unsigned int width, unsigned int height)
{
	UINT32 pixel_count = width * height;
	UINT32 i;
	static UINT32 cnt = 0;

	for (i = 0; i < pixel_count; i++) {
		dest_addr[i] = (UINT8)(src_addr[i] >> 8);
	}

	if (g_dbg_print && dbg_file_path != NULL) {
		FILE *fp;
		char dbg_raw_file_path[256];

		snprintf(dbg_raw_file_path, sizeof(dbg_raw_file_path),
			 "%s_raw_%d_%d_8bit_%d.raw",
			 dbg_file_path, width, height, cnt);
		fp = fopen(dbg_raw_file_path, "wb");
		if (fp == NULL) {
			printf("open %s fail \r\n", dbg_raw_file_path);
		} else {
			fwrite(dest_addr, 1, pixel_count, fp);
			fflush(fp);
			usleep(100000);
			fclose(fp);
		}
		cnt++;
	}
}

/*
 * 從 ISP 抓出一張 RAW frame 到 raw_buf。
 *
 * 主要流程：
 *   1. 暫時關閉 ECS/shading。
 *   2. 用 ISPT_ITEM_RAW 取得 RAW frame 的格式、寬高與起始位址。
 *   3. 拒絕壓縮 RAW 格式，只接受未壓縮 RAW8 / RAW16。
 *   4. 用 ISPT_ITEM_FRAME 每次搬 32 KB，直到整張 RAW 搬完。
 *   5. 通知 ISP RAW 讀取完成，並恢復 ECS/shading。
 *
 * 輸出：
 *   final_raw_info 會帶回 RAW 格式、寬高、line offset 等資訊。
 *   raw_buf 會被填入實際 RAW 影像資料。
 */
unsigned int dpc_get_raw(int sensor_id, ISPT_RAW_INFO *final_raw_info, UINT32* raw_buf)
{
	HD_RESULT ret = HD_OK;
	HD_RESULT cleanup_ret;
	unsigned int result = (unsigned int)-1;
	unsigned int data_len, residue_len, data;
	unsigned int image_width, image_height, j, block;
	unsigned int image_size = 1920*1080;
	UINT32 ecs_enable = 0;
	BOOL dbg_save_raw = FALSE;
	char dbg_raw_file_path[256];
	char *img_buff = (char *)(raw_buf);
	FILE *fp = NULL;
	int ecs_bypassed = 0;
	int raw_acquired = 0;
	static ISPT_MEMORY_INFO mem_info = {0};
	static int g_file_idx = 0;

	if (final_raw_info == NULL || raw_buf == NULL) {
		printf("invalid DPC RAW output buffer\n");
		return result;
	}

	final_raw_info->id = sensor_id;
	if (bypass_ecs(sensor_id, &ecs_enable) < 0) {
		return result;
	}
	ecs_bypassed = 1;
	g_file_idx++;

	//get raw
	ret = vendor_isp_get_common(ISPT_ITEM_RAW, final_raw_info);
	if (ret < 0) {
		printf("GET ISPT_ITEM_RAW fail! \n");
		goto cleanup;
	}
	raw_acquired = 1;
	if((g_dbg_print)&&(dbg_file_path != NULL)){
		dbg_save_raw = TRUE;
		printf("dbg save raw on \n");
	}

	final_raw_info->raw_info.pxlfmt = final_raw_info->raw_info.pxlfmt & 0xffff0000;
	if(g_dbg_print)
		printf("get raw, id = %d, %d x %d x %x\n", final_raw_info->id, final_raw_info->raw_info.pw, final_raw_info->raw_info.ph, final_raw_info->raw_info.pxlfmt);

	if (final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_NRX12 ||
		final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_NRX14) {
		printf("This is compressed raw format. Only support uncompressed raw format\r\n");
		goto cleanup;
	}

	if(g_dbg_print)
		printf("get lineoffset = %d  \r\n", final_raw_info->raw_info.loff);

	image_width = final_raw_info->raw_info.pw;
	image_height = final_raw_info->raw_info.ph;
	if(final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW16){
		image_size = image_width * image_height * 2;
	}else if(final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW8){
		image_size = image_width * image_height;
	}else{
		printf("Unsupported raw format: 0x%x\r\n", final_raw_info->raw_info.pxlfmt);
		goto cleanup;
	}
	
	if ((image_width | image_height) == 0) {
		printf("image_width/image_height can not be zero \r\n");
		goto cleanup;
	}

	// allocate raw buffer

	if (img_buff == NULL) {
		printf("fail to allocate memory for image buffer!\n");
		goto cleanup;
	}

	if(final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW16){
		memset(img_buff, 0, image_width*image_height*2);
	}else if(final_raw_info->raw_info.pxlfmt == VDO_PXLFMT_RAW8){
		memset(img_buff, 0, image_width*image_height);
	}

	

	block = image_size % (32 * 1024) ? (image_size / 1024 / 32 + 1) : (image_size / 1024 / 32);
	printf("block %d \r\n", block);

	//move raw
	data_len = 32*1024;
	mem_info.addr = final_raw_info->raw_info.addr;
	residue_len = image_size;
	data = 0;


	for (j = 0; j < block ; j++) {
	
		mem_info.size = data_len;
			ret = vendor_isp_get_common(ISPT_ITEM_FRAME, &mem_info);
		//printf("finish get frame \r\n");
		if (ret < 0) {
			printf("GET ISPT_ITEM_FRAME fail! \n");
			goto cleanup;
		}

		memcpy(&img_buff[data], mem_info.buf, data_len);

		mem_info.addr += 32*1024;
		residue_len -= data_len;
		data += data_len;
		data_len = (residue_len >= 32 * 1024) ? 32 * 1024 : residue_len;
	}

	//set raw
	ret = vendor_isp_set_common(ISPT_ITEM_RAW, &final_raw_info->id);
	raw_acquired = 0;
	if (ret < 0) {
		printf("SET ISPT_ITEM_RAW fail! \n");
		goto cleanup;
	}


	if(dbg_save_raw){

		snprintf(dbg_raw_file_path, 256, "%s_raw_%d_%d_16bit_%d.raw",dbg_file_path,image_width,image_height,g_file_idx);
		printf("save file: %s \n", dbg_raw_file_path);
		fp = fopen(dbg_raw_file_path, "wb");
		if(fp == NULL){
			printf("open %s fail \r\n",dbg_raw_file_path);
		}else{
			fwrite(img_buff, 1, image_size, fp);				
			fflush(fp);
		}
		if (fp != NULL) {
			fclose(fp);
			fp = NULL;
		}
	
	}



	result = 0U;

cleanup:
	if (fp != NULL) {
		fclose(fp);
	}
	if (raw_acquired) {
		cleanup_ret = vendor_isp_set_common(
			ISPT_ITEM_RAW, &final_raw_info->id);
		if (cleanup_ret < 0) {
			printf("SET ISPT_ITEM_RAW fail during cleanup! \n");
			result = (unsigned int)-1;
		}
	}
	if (ecs_bypassed) {
		if (restore_ecs(sensor_id, ecs_enable) < 0) {
			printf("restore ECS fail during DPC RAW cleanup! \n");
			result = (unsigned int)-1;
		}
	}

	return result;
}


/*
 * DPC sample 主流程。
 *
 * 選單功能：
 *   1. Start bright/dark BPR cali
 *      分兩次抓 RAW：先偵測 bright pixel，再偵測 dark pixel，
 *      最後把兩種壞點合併到同一份 DPC table。
 *
 *   2. load DPC table
 *      重新載入 ISP cfg，讓 cfg 中設定的 DPC table 有機會被 ISP 載入。
 *
 *   3. Dbg mode on/off
 *      開關 DPC library debug log，並可把 RAW dump 到 /mnt/sd/。
 */
int bpr_menu_run(void)
{
	unsigned int option, infinite_loop = 1;

	unsigned int dpc_th = g_bpr_threshold;
	unsigned int dbg_en = 0;
	IQT_CFG_INFO cfg_info = {0};

	int ret = 0;
	static char dbg_raw_file_path[64] = "/mnt/sd/";

	UINT32 max_cnt = BPR_MAX_DP_COUNT;

	printf("----------------------------------------\n");
	printf("sample 2.10, lib: %x\r\n",get_dpc_lib_version());
	printf("thermal_pipeline_id=%u\r\n", g_thermal_pipeline_id);
	cfg_info.id = g_thermal_pipeline_id;
	dpc_param.id = g_thermal_pipeline_id;



	while (infinite_loop) {
		printf("----------------------------------------\n");
		printf("BPR menu\n");
		printf(" 1. Bright/Dark BPR calibration\n");
		printf(" 2. Reload BPR table\n");
		printf(" 3. BPR debug on/off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: BPR=%s, threshold=%u%%, Runtime BPR=%s, threshold=%u%%\r\n",
		       bpr_is_enabled() ? "on" : "off",
		       bpr_get_threshold(),
		       bpr_is_runtime_enabled() ? "on" : "off",
		       bpr_get_runtime_threshold());
		printf("Settings: CFG Setting -> BPR setting\r\n");

		do {
			printf(">> ");
			option = get_choose_uint();
		} while (0);

		switch (option) {
		case 0:
			infinite_loop = 0;
			break;

		case 1:
			dpc_th = bpr_get_threshold();
			ret = dpc_run_bright_dark(g_thermal_pipeline_id,
						 dpc_th, max_cnt);
			if (ret < 0) {
				printf("bright/dark BPR fail=%d\n", ret);
			}
			break;


		case 2:
			// 重新載入 ISP cfg，讓 cfg 裡設定的 DPC table 路徑可以生效。
			ret = dpc_load_cfg(&cfg_info);
			//printf("dpc_param 0x%x 0x%x\e\n",dpc_param.dpc.table[0],dpc_param.dpc.table[1]);
			break;

		case 3:
			// 開啟 debug log，以及這個範例/library 的 RAW dump 功能。
			do {
				printf("BPR debug (0: off, 1: on)\r\n");
				printf(">> ");
				dbg_en = get_choose_uint();
				if (dbg_en > 1U) {
					printf("Invalid BPR debug setting, enter 0 or 1\r\n");
				}
			} while (dbg_en > 1U);
			cali_dpc_set_dbg_out(dbg_en);
			g_dbg_print =dbg_en;
			dbg_file_path = dbg_raw_file_path ;
			break;

		default:
			infinite_loop = 0;
			break;
		}
    }

	return ret;
}

