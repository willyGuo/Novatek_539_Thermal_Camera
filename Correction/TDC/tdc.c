#include <limits.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__LINUX)
#include <fcntl.h>
#endif
#include "tdc.h"
#include <inttypes.h>
#include "../../Device/SensorTemp/sensor_temp.h"
#include "vendor_gfx.h"

#define TDC_DIR "/mnt/sd/thermal_op"
#define TDC_FILE_DCC "/mnt/sd/thermal_op/DCC_value.bin"
#define TDC_FILE_LC "/mnt/sd/thermal_op/LC_value.bin"

#define TDC_REFERENCE_TEMPERATURE_C 25.0f
#define TDC_CALIBRATION_FRAME_COUNT 4U
#define TDC_MIN_TEMPERATURE_DELTA_C 0.1f
#define TDC_DEFAULT_INTERVAL_SEC 60U
#define MENU_OPTION_REFRESH (-2)

typedef int (*TDC_READ_TEMPERATURE_CB)(float *temp_c);

typedef struct {
	HD_COMMON_MEM_VB_BLK DCC_K_blk;
	UINTPTR DCC_K_pa;
	UINTPTR DCC_K_va;
	HD_COMMON_MEM_VB_BLK LC_K_blk;
	UINTPTR LC_K_pa;
	UINTPTR LC_K_va;
	HD_COMMON_MEM_VB_BLK ADD_output_blk;
	UINTPTR ADD_output_pa;
	UINTPTR ADD_output_va;
	HD_COMMON_MEM_VB_BLK all_1_blk;
	UINTPTR all_1_pa;
	UINTPTR all_1_va;
} TDC_MEM;

static TDC_MEM tdc_buffer[1];
static TDC_IMAGE_LAYOUT g_tdc_layout;
static TDC_CAPTURE_RAW_CB g_tdc_capture_raw;
static int g_tdc_enabled;
static int g_tdc_sensor_table_loaded;
static int g_tdc_lens_table_loaded;
static UINT32 g_tdc_interval_sec = TDC_DEFAULT_INTERVAL_SEC;

static int tdc_sensor_calibrate(void);
static int tdc_lens_calibrate(void);
static int tdc_reload_table(void);

static int tdc_validate_image_layout(void)
{
	if (g_tdc_layout.raw_width == 0 || g_tdc_layout.raw_height == 0 ||
	    g_tdc_layout.active_width == 0 ||
	    g_tdc_layout.active_height == 0 ||
	    g_tdc_layout.active_x >= g_tdc_layout.raw_width ||
	    g_tdc_layout.active_y >= g_tdc_layout.raw_height ||
	    g_tdc_layout.active_width >
		g_tdc_layout.raw_width - g_tdc_layout.active_x ||
	    g_tdc_layout.active_height >
		g_tdc_layout.raw_height - g_tdc_layout.active_y ||
	    (UINT64)g_tdc_layout.raw_width * g_tdc_layout.raw_height >
		(UINT32_MAX / sizeof(float))) {
		printf("TDC image layout is invalid\n");
		return -1;
	}

	return 0;
}

static UINT32 tdc_raw_pixel_count(void)
{
	return g_tdc_layout.raw_width * g_tdc_layout.raw_height;
}

static UINT32 tdc_slope_table_size(void)
{
	return tdc_raw_pixel_count() * sizeof(float);
}

static UINT32 tdc_correction_table_size(void)
{
	return tdc_raw_pixel_count() * sizeof(int16_t);
}

static int tdc_table_is_loaded(void)
{
	return g_tdc_sensor_table_loaded && g_tdc_lens_table_loaded;
}

static void tdc_disable_for_missing_table(void)
{
	g_tdc_enabled = 0;
	printf("TDC table is incomplete, keep TDC off\n");
}

static int tdc_make_output_dir(void)
{
	if (mkdir(TDC_DIR, 0755) != 0 && access(TDC_DIR, F_OK) != 0) {
		printf("create TDC table dir (%s) fail\n", TDC_DIR);
		return -1;
	}

	return 0;
}

static int tdc_read_table_file(const char *path, void *table, UINT32 size)
{
	FILE *fp;
	struct stat file_stat;
	UINT32 read_size;

	if (stat(path, &file_stat) != 0) {
		printf("TDC table file missing: %s\n", path);
		return -1;
	}
	if ((UINT64)file_stat.st_size != size) {
		printf("TDC table (%s) size error, size=%lld expect=%u\n",
		       path, (long long)file_stat.st_size, size);
		return -1;
	}

	fp = fopen(path, "rb");
	if (fp == NULL) {
		printf("open TDC table (%s) fail\n", path);
		return -1;
	}

	read_size = (UINT32)fread(table, 1, size, fp);
	fclose(fp);
	if (read_size != size) {
		printf("read TDC table (%s) size error, read=%u expect=%u\n",
		       path, read_size, size);
		return -1;
	}

	return 0;
}

static int tdc_menu_read_option(void)
{
	char buf[32];
	char *end;
	unsigned long value;

	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == NULL) {
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

static int tdc_show_sensor_temperature(void)
{
	float temp_c;

	if (sensor_temp_read(&temp_c) != 0) {
		printf("Read Sensor Temperature fail\n");
		return -1;
	}

	printf("Current Sensor Temperature = %.4f C\n", temp_c);
	return 0;
}

static int tdc_show_lens_temperature(void)
{
	float temp_c;

	if (lens_temp_read(&temp_c) != 0) {
		printf("Read Lens Temperature fail\n");
		return -1;
	}

	printf("Current Lens Temperature = %.4f C\n", temp_c);
	return 0;
}

int tdc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("TDC menu\n");
		printf(" 1. Sensor TDC Calibration\n");
		printf(" 2. Lens TDC Calibration\n");
		printf(" 3. Reload\n");
		printf(" 4. Show Current Sensor Temperature\n");
		printf(" 5. Show Current Lens Temperature\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: %s, Sensor table=%s, Lens table=%s, interval=%u sec\n",
		       g_tdc_enabled ? "on" : "off",
		       g_tdc_sensor_table_loaded ? "loaded" : "not loaded",
		       g_tdc_lens_table_loaded ? "loaded" : "not loaded",
		       g_tdc_interval_sec);
		printf("Settings: CFG Setting -> TDC setting\n");

		option = tdc_menu_read_option();
		switch (option) {
		case 1:
			ret = tdc_sensor_calibrate();
			break;

		case 2:
			ret = tdc_lens_calibrate();
			break;

		case 3:
			ret = tdc_reload_table();
			break;

		case 4:
			ret = tdc_show_sensor_temperature();
			break;

		case 5:
			ret = tdc_show_lens_temperature();
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

void tdc_bind_runtime(const TDC_IMAGE_LAYOUT *layout)
{
	if (layout != NULL) {
		g_tdc_layout = *layout;
	} else {
		memset(&g_tdc_layout, 0, sizeof(g_tdc_layout));
	}
}

void tdc_bind_capture_callback(TDC_CAPTURE_RAW_CB capture_raw)
{
	g_tdc_capture_raw = capture_raw;
}

void tdc_set_enabled(int enable)
{
	if (enable && !tdc_table_is_loaded()) {
		tdc_disable_for_missing_table();
		return;
	}

	g_tdc_enabled = enable ? 1 : 0;
	printf("TDC %s\n", g_tdc_enabled ? "on" : "off");
}

int tdc_is_enabled(void)
{
	return g_tdc_enabled && tdc_table_is_loaded();
}

void tdc_set_interval_sec(UINT32 interval_sec)
{
	if (interval_sec == 0) {
		printf("TDC interval must be greater than 0 sec\n");
		return;
	}

	g_tdc_interval_sec = interval_sec;
	printf("TDC interval=%u sec\n", g_tdc_interval_sec);
}

UINT32 tdc_get_interval_sec(void)
{
	return g_tdc_interval_sec;
}

static int tdc_wait_for_temperature_point(const char *calibration_name,
					   const char *point_name)
{
	char buf[16];

	printf("\n%s TDC %s point.\n", calibration_name, point_name);
	printf("Keep the blackbody temperature fixed.\n");
	printf("Keep the non-target temperature source stable.\n");
	printf("Wait for the %s temperature to stabilize, then press Enter.\n",
	       calibration_name);
	printf(">> ");

	return fgets(buf, sizeof(buf), stdin) == NULL ? -1 : 0;
}

static int tdc_capture_temperature_point(
	const char *calibration_name,
	const char *point_name,
	TDC_READ_TEMPERATURE_CB read_temperature,
	UINT16 *average_raw,
	UINT16 *frame_raw,
	UINT32 *raw_sum,
	float *average_temp_c)
{
	float temperature_sum = 0.0f;
	UINT32 raw_pixel_count;
	UINT32 frame;
	UINT32 x;
	UINT32 y;

	if (tdc_validate_image_layout() != 0 ||
	    tdc_wait_for_temperature_point(calibration_name, point_name) != 0) {
		return -1;
	}

	raw_pixel_count = tdc_raw_pixel_count();
	memset(raw_sum, 0, raw_pixel_count * sizeof(*raw_sum));

	for (frame = 0; frame < TDC_CALIBRATION_FRAME_COUNT; frame++) {
		float temp_c;

		if (read_temperature(&temp_c) != 0) {
			printf("read %s temperature fail at %s frame %u\n",
			       calibration_name, point_name, frame + 1);
			return -1;
		}
		if (g_tdc_capture_raw((UINTPTR)frame_raw,
				      raw_pixel_count * sizeof(*frame_raw)) != 0) {
			printf("capture %s TDC %s frame %u fail\n",
			       calibration_name, point_name, frame + 1);
			return -1;
		}

		temperature_sum += temp_c;
		for (y = 0; y < g_tdc_layout.active_height; y++) {
			UINT32 row = (g_tdc_layout.active_y + y) *
				g_tdc_layout.raw_width + g_tdc_layout.active_x;

			for (x = 0; x < g_tdc_layout.active_width; x++) {
				raw_sum[row + x] += frame_raw[row + x];
			}
		}

		printf("%s TDC %s frame %u/%u, temperature=%.4f C\n",
		       calibration_name, point_name, frame + 1,
		       TDC_CALIBRATION_FRAME_COUNT, temp_c);
	}

	memset(average_raw, 0, raw_pixel_count * sizeof(*average_raw));
	for (y = 0; y < g_tdc_layout.active_height; y++) {
		UINT32 row = (g_tdc_layout.active_y + y) *
			g_tdc_layout.raw_width + g_tdc_layout.active_x;

		for (x = 0; x < g_tdc_layout.active_width; x++) {
			average_raw[row + x] = (UINT16)(
				(raw_sum[row + x] +
				 TDC_CALIBRATION_FRAME_COUNT / 2U) /
				TDC_CALIBRATION_FRAME_COUNT);
		}
	}
	*average_temp_c =
		temperature_sum / (float)TDC_CALIBRATION_FRAME_COUNT;

	printf("%s TDC %s average temperature = %.4f C\n",
	       calibration_name, point_name, *average_temp_c);
	return 0;
}

static int tdc_save_temperature_table(const char *calibration_name,
				      const char *table_path,
				      const float *temperature_k)
{
	char temporary_path[128];
	FILE *fp = NULL;
	UINT32 slope_table_size = tdc_slope_table_size();
	UINT32 write_size;
	int saved_errno;

	if (snprintf(temporary_path, sizeof(temporary_path), "%s.tmp",
		     table_path) >= (int)sizeof(temporary_path)) {
		printf("%s TDC table path is too long\n", calibration_name);
		return -1;
	}
	if (tdc_make_output_dir() != 0) {
		return -1;
	}

	fp = fopen(temporary_path, "wb");
	if (fp == NULL) {
		printf("open %s TDC table (%s) fail\n",
		       calibration_name, temporary_path);
		return -1;
	}

	write_size = (UINT32)fwrite(temperature_k, 1,
				    slope_table_size, fp);
	if (write_size != slope_table_size || ferror(fp)) {
		printf("write %s TDC table size error, write=%u expect=%u\n",
		       calibration_name, write_size,
		       slope_table_size);
		goto fail;
	}
	if (fflush(fp) != 0) {
		printf("flush %s TDC table fail: %s\n",
		       calibration_name, strerror(errno));
		goto fail;
	}
#if defined(__LINUX)
	if (fsync(fileno(fp)) != 0) {
		printf("sync %s TDC table fail: %s\n",
		       calibration_name, strerror(errno));
		goto fail;
	}
#endif
	if (fclose(fp) != 0) {
		fp = NULL;
		printf("close %s TDC table fail: %s\n",
		       calibration_name, strerror(errno));
		goto fail;
	}
	fp = NULL;
	if (rename(temporary_path, table_path) != 0) {
		printf("replace %s TDC table (%s) fail: %s\n",
		       calibration_name, table_path, strerror(errno));
		goto fail;
	}

#if defined(__LINUX)
	{
		int dir_fd = open(TDC_DIR, O_RDONLY);

		if (dir_fd < 0) {
			printf("warning: open TDC table directory fail: %s\n",
			       strerror(errno));
		} else {
			if (fsync(dir_fd) != 0) {
				printf("warning: sync TDC table directory fail: %s\n",
				       strerror(errno));
			}
			close(dir_fd);
		}
	}
#endif

	return 0;

fail:
	saved_errno = errno;
	if (fp != NULL) {
		fclose(fp);
	}
	if (remove(temporary_path) != 0 && errno != ENOENT) {
		printf("remove incomplete %s TDC table fail: %s\n",
		       calibration_name, strerror(errno));
	}
	errno = saved_errno;
	return -1;
}

static int tdc_temperature_calibrate(
	const char *calibration_name,
	TDC_READ_TEMPERATURE_CB read_temperature,
	const char *table_path,
	UINTPTR table_va,
	int *table_loaded)
{
	UINT16 *low_average = NULL;
	UINT16 *high_average = NULL;
	UINT16 *frame_raw = NULL;
	UINT32 *raw_sum = NULL;
	float *temperature_k;
	float low_temp_c;
	float high_temp_c;
	float temperature_delta_c;
	UINT32 raw_pixel_count;
	UINT32 slope_table_size;
	UINT32 x;
	UINT32 y;
	int ret = -1;

	if (tdc_validate_image_layout() != 0) {
		return -1;
	}
	if (table_va == 0) {
		printf("%s TDC buffer is not ready; start liveview first\n",
		       calibration_name);
		return -1;
	}
	if (g_tdc_capture_raw == NULL) {
		printf("%s TDC capture is not ready; start liveview first\n",
		       calibration_name);
		return -1;
	}

	raw_pixel_count = tdc_raw_pixel_count();
	slope_table_size = tdc_slope_table_size();
	low_average = (UINT16 *)malloc(raw_pixel_count * sizeof(*low_average));
	high_average = (UINT16 *)malloc(raw_pixel_count * sizeof(*high_average));
	frame_raw = (UINT16 *)malloc(raw_pixel_count * sizeof(*frame_raw));
	raw_sum = (UINT32 *)malloc(raw_pixel_count * sizeof(*raw_sum));
	if (low_average == NULL || high_average == NULL ||
	    frame_raw == NULL || raw_sum == NULL) {
		printf("%s TDC calibration malloc fail\n", calibration_name);
		goto exit;
	}

	if (tdc_capture_temperature_point(
		    calibration_name, "low", read_temperature,
		    low_average, frame_raw, raw_sum, &low_temp_c) != 0) {
		goto exit;
	}
	if (tdc_capture_temperature_point(
		    calibration_name, "high", read_temperature,
		    high_average, frame_raw, raw_sum, &high_temp_c) != 0) {
		goto exit;
	}

	temperature_delta_c = high_temp_c - low_temp_c;
	if (fabsf(temperature_delta_c) < TDC_MIN_TEMPERATURE_DELTA_C) {
		printf("%s TDC temperature delta is too small: %.4f C\n",
		       calibration_name, temperature_delta_c);
		goto exit;
	}

	/*
	 * raw_sum is no longer needed as an integer accumulator. Reuse its
	 * storage for the per-pixel TDC slope in ADC/degree C.
	 */
	temperature_k = (float *)raw_sum;
	memset(temperature_k, 0, slope_table_size);
	for (y = 0; y < g_tdc_layout.active_height; y++) {
		UINT32 row = (g_tdc_layout.active_y + y) *
			g_tdc_layout.raw_width + g_tdc_layout.active_x;

		for (x = 0; x < g_tdc_layout.active_width; x++) {
			UINT32 pixel = row + x;

			temperature_k[pixel] =
				((float)high_average[pixel] -
				 (float)low_average[pixel]) /
				temperature_delta_c;
		}
	}

	if (tdc_save_temperature_table(calibration_name, table_path,
				       temperature_k) != 0) {
		goto exit;
	}

	memcpy((void *)table_va, temperature_k, slope_table_size);
	hd_common_mem_flush_cache((void *)table_va, slope_table_size);
	*table_loaded = 1;

	printf("%s TDC calibration done: low=%.4f C, high=%.4f C, "
	       "delta=%.4f C\n",
	       calibration_name, low_temp_c, high_temp_c, temperature_delta_c);
	printf("%s TDC table saved: %s\n", calibration_name, table_path);
	if (tdc_table_is_loaded()) {
		g_tdc_enabled = 1;
		printf("Sensor/Lens TDC tables are ready, TDC on\n");
	} else {
		tdc_disable_for_missing_table();
	}
	ret = 0;

exit:
	free(low_average);
	free(high_average);
	free(frame_raw);
	free(raw_sum);
	return ret;
}

static int tdc_reload_table(void)
{
	float *sensor_table = NULL;
	float *lens_table = NULL;
	UINT32 slope_table_size;
	int ret = -1;

	if (tdc_validate_image_layout() != 0) {
		return -1;
	}
	if (tdc_buffer->DCC_K_va == 0 || tdc_buffer->LC_K_va == 0) {
		printf("TDC table buffer is not ready; start liveview first\n");
		return -1;
	}

	slope_table_size = tdc_slope_table_size();
	sensor_table = (float *)malloc(slope_table_size);
	lens_table = (float *)malloc(slope_table_size);
	if (sensor_table == NULL || lens_table == NULL) {
		printf("TDC reload table malloc fail\n");
		goto exit;
	}

	if (tdc_read_table_file(TDC_FILE_DCC, sensor_table,
				slope_table_size) != 0 ||
	    tdc_read_table_file(TDC_FILE_LC, lens_table,
				slope_table_size) != 0) {
		printf("TDC reload failed; keep current tables\n");
		goto exit;
	}

	memcpy((void *)tdc_buffer->DCC_K_va, sensor_table,
	       slope_table_size);
	memcpy((void *)tdc_buffer->LC_K_va, lens_table,
	       slope_table_size);
	hd_common_mem_flush_cache((void *)tdc_buffer->DCC_K_va,
				  slope_table_size);
	hd_common_mem_flush_cache((void *)tdc_buffer->LC_K_va,
				  slope_table_size);

	g_tdc_sensor_table_loaded = 1;
	g_tdc_lens_table_loaded = 1;
	g_tdc_enabled = 1;
	printf("Reload Sensor/Lens TDC tables ok, TDC on\n");
	ret = 0;

exit:
	free(sensor_table);
	free(lens_table);
	return ret;
}

static int tdc_sensor_calibrate(void)
{
	return tdc_temperature_calibrate(
		"Sensor", sensor_temp_read, TDC_FILE_DCC,
		tdc_buffer->DCC_K_va, &g_tdc_sensor_table_loaded);
}

static int tdc_lens_calibrate(void)
{
	return tdc_temperature_calibrate(
		"Lens", lens_temp_read, TDC_FILE_LC,
		tdc_buffer->LC_K_va, &g_tdc_lens_table_loaded);
}


HD_RESULT init_thermal_TDC_mem(void)
{
	int16_t *all_1_add;
	UINT32 raw_pixel_count;
	UINT32 slope_table_size;
	UINT32 correction_table_size;
	UINT32 i;

	if (tdc_validate_image_layout() != 0) {
		return HD_ERR_NG;
	}
	raw_pixel_count = tdc_raw_pixel_count();
	slope_table_size = tdc_slope_table_size();
	correction_table_size = tdc_correction_table_size();

	g_tdc_enabled = 0;
	g_tdc_sensor_table_loaded = 0;
	g_tdc_lens_table_loaded = 0;

	printf("TDC slope table size=%u, correction table size=%u\n",
	       slope_table_size, correction_table_size);

	tdc_buffer->DCC_K_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, slope_table_size, DDR_ID0);
	if (tdc_buffer->DCC_K_blk == 0) {
		printf("get block fail (0x%" PRIu64 ")\r\n", (uint64_t)tdc_buffer->DCC_K_blk);
		return HD_ERR_NG;
	}
	tdc_buffer->DCC_K_pa = hd_common_mem_blk2pa(tdc_buffer->DCC_K_blk);
	if (tdc_buffer->DCC_K_pa == 0) {
		printf("blk2pa fail, DCC_K_blk = 0x%" PRIu64 "\r\n",  (uint64_t)tdc_buffer->DCC_K_pa);
		return HD_ERR_NG;
	}
	tdc_buffer->DCC_K_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, tdc_buffer->DCC_K_pa,
		slope_table_size);
	if (tdc_buffer->DCC_K_va == 0) {
		printf("Error: mmap pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)tdc_buffer->DCC_K_va, 0, slope_table_size);

	tdc_buffer->LC_K_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, slope_table_size, DDR_ID0);
	if (tdc_buffer->LC_K_blk == 0) {
		printf("get block fail (0x%" PRIu64 ")\r\n", (uint64_t)tdc_buffer->LC_K_blk);
		return HD_ERR_NG;
	}

	tdc_buffer->LC_K_pa = hd_common_mem_blk2pa(tdc_buffer->LC_K_blk);
	if (tdc_buffer->LC_K_pa == 0) {
		printf("blk2pa fail, LC_K_blk = 0x%" PRIu64 "\r\n",  (uint64_t)tdc_buffer->LC_K_pa);
		return HD_ERR_NG;
	}

	tdc_buffer->LC_K_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, tdc_buffer->LC_K_pa,
		slope_table_size);
	if (tdc_buffer->LC_K_va == 0) {
		printf("Error: mmap pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)tdc_buffer->LC_K_va, 0, slope_table_size);

	tdc_buffer->ADD_output_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, correction_table_size, DDR_ID0);
	if (tdc_buffer->ADD_output_blk == 0) {
		printf("get block fail (0x%" PRIu64 ")\r\n", (uint64_t)tdc_buffer->ADD_output_blk);
		return HD_ERR_NG;
	}

	tdc_buffer->ADD_output_pa =
		hd_common_mem_blk2pa(tdc_buffer->ADD_output_blk);
	if (tdc_buffer->ADD_output_pa == 0) {
		printf("blk2pa fail, ADD_output_blk = 0x%" PRIu64 "\r\n",  (uint64_t)tdc_buffer->ADD_output_pa);
		return HD_ERR_NG;
	}

	tdc_buffer->ADD_output_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, tdc_buffer->ADD_output_pa,
		correction_table_size);
	if (tdc_buffer->ADD_output_va == 0) {
		printf("Error: mmap pa fail !\r\n");
		return HD_ERR_NG;
	}
	memset((void *)tdc_buffer->ADD_output_va, 0, correction_table_size);
	hd_common_mem_flush_cache((void *)tdc_buffer->ADD_output_va,
				  correction_table_size);
	tdc_buffer->all_1_blk = hd_common_mem_get_block(
		HD_COMMON_MEM_COMMON_POOL, correction_table_size, DDR_ID0);
	if (tdc_buffer->all_1_blk == 0) {
		printf("get block fail (0x%" PRIu64 ")\r\n", (uint64_t)tdc_buffer->all_1_blk);
		return HD_ERR_NG;
	}

	tdc_buffer->all_1_pa = hd_common_mem_blk2pa(tdc_buffer->all_1_blk);
	if (tdc_buffer->all_1_pa == 0) {
		printf("blk2pa fail, all_1_blk = 0x%" PRIu64 "\r\n",  (uint64_t)tdc_buffer->all_1_pa);
		return HD_ERR_NG;
	}

	tdc_buffer->all_1_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, tdc_buffer->all_1_pa,
		correction_table_size);
	if (tdc_buffer->all_1_va == 0) {
		printf("Error: mmap pa fail !\r\n");
		return HD_ERR_NG;
	}
	all_1_add = (int16_t *)tdc_buffer->all_1_va;
	for (i = 0; i < raw_pixel_count; i++) {
		all_1_add[i] = 1;
	}
	hd_common_mem_flush_cache((void *)tdc_buffer->all_1_va,
				  correction_table_size);
	
	if (tdc_read_table_file(TDC_FILE_DCC,
				(void *)tdc_buffer->DCC_K_va,
				slope_table_size) == 0) {
		hd_common_mem_flush_cache((void *)tdc_buffer->DCC_K_va,
					  slope_table_size);
		g_tdc_sensor_table_loaded = 1;
		printf("Sensor TDC table loaded: %s\n", TDC_FILE_DCC);
	} else {
		memset((void *)tdc_buffer->DCC_K_va, 0, slope_table_size);
		hd_common_mem_flush_cache((void *)tdc_buffer->DCC_K_va,
					  slope_table_size);
		printf("Use zero Sensor TDC table until calibration\n");
	}

	if (tdc_read_table_file(TDC_FILE_LC,
				(void *)tdc_buffer->LC_K_va,
				slope_table_size) == 0) {
		hd_common_mem_flush_cache((void *)tdc_buffer->LC_K_va,
					  slope_table_size);
		g_tdc_lens_table_loaded = 1;
		printf("Lens TDC table loaded: %s\n", TDC_FILE_LC);
	} else {
		memset((void *)tdc_buffer->LC_K_va, 0, slope_table_size);
		hd_common_mem_flush_cache((void *)tdc_buffer->LC_K_va,
					  slope_table_size);
		printf("Use zero Lens TDC table until calibration\n");
	}

	if (tdc_table_is_loaded()) {
		g_tdc_enabled = 1;
		printf("Sensor/Lens TDC tables are ready, TDC on\n");
	} else {
		tdc_disable_for_missing_table();
	}
	return HD_OK;
}


HD_RESULT exit_TDC_mem(void)
{
	UINT32 slope_table_size = tdc_slope_table_size();
	UINT32 correction_table_size = tdc_correction_table_size();

	printf("Cleaning up TDC resources...\n");

	if (tdc_buffer->DCC_K_va && tdc_buffer->DCC_K_va != (UINTPTR)-1) {
		hd_common_mem_munmap((void *)tdc_buffer->DCC_K_va,
				    slope_table_size);
	}
	if (tdc_buffer->LC_K_va && tdc_buffer->LC_K_va != (UINTPTR)-1) {
		hd_common_mem_munmap((void *)tdc_buffer->LC_K_va,
				    slope_table_size);
	}
	if (tdc_buffer->ADD_output_va && tdc_buffer->ADD_output_va != (UINTPTR)-1) {
		hd_common_mem_munmap((void *)tdc_buffer->ADD_output_va,
				    correction_table_size);
	}
	if (tdc_buffer->all_1_va && tdc_buffer->all_1_va != (UINTPTR)-1) {
		hd_common_mem_munmap((void *)tdc_buffer->all_1_va,
				    correction_table_size);
	}
	if (tdc_buffer->DCC_K_blk) {
		hd_common_mem_release_block(tdc_buffer->DCC_K_blk);
	}
	if (tdc_buffer->LC_K_blk) {
		hd_common_mem_release_block(tdc_buffer->LC_K_blk);
	}
	if (tdc_buffer->ADD_output_blk) {
		hd_common_mem_release_block(tdc_buffer->ADD_output_blk);
	}
	if (tdc_buffer->all_1_blk) {
		hd_common_mem_release_block(tdc_buffer->all_1_blk);
	}
	memset(tdc_buffer, 0, sizeof(*tdc_buffer));
	g_tdc_sensor_table_loaded = 0;
	g_tdc_lens_table_loaded = 0;
	g_tdc_enabled = 0;

	return HD_OK;
}

static int16_t tdc_clip_correction(float correction)
{
	if (correction > 32767.0f) {
		return 32767;
	}
	if (correction < -32768.0f) {
		return -32768;
	}

	return (int16_t)lroundf(correction);
}

HD_RESULT tdc_update_correction_table(void)
{
	float Temp_LC;
	float Temp_DCC;
	float diff_Temp_LC;
	float diff_Temp_DCC;
	UINT32 pixel_count;
	UINT32 correction_table_size;
	const float *dcc_k_ptr;
	const float *lc_k_ptr;
	int16_t *out_ptr;
	UINT32 i;

	if (tdc_validate_image_layout() != 0) {
		return HD_ERR_NG;
	}
	if (tdc_buffer->DCC_K_va == 0 || tdc_buffer->LC_K_va == 0 ||
	    tdc_buffer->ADD_output_va == 0) {
		printf("TDC update buffer is not ready\n");
		return HD_ERR_NG;
	}
	pixel_count = tdc_raw_pixel_count();
	correction_table_size = tdc_correction_table_size();
	
	if (sensor_temp_read(&Temp_DCC) != 0) {
		printf("TDC read sensor temperature fail\n");
		return HD_ERR_NG;
	}
	diff_Temp_DCC = Temp_DCC - TDC_REFERENCE_TEMPERATURE_C;

	if (lens_temp_read(&Temp_LC) != 0) {
		printf("TDC read lens temperature fail\n");
		return HD_ERR_NG;
	}
	diff_Temp_LC = Temp_LC - TDC_REFERENCE_TEMPERATURE_C;

	dcc_k_ptr = (const float *)tdc_buffer->DCC_K_va;
	lc_k_ptr = (const float *)tdc_buffer->LC_K_va;
	out_ptr = (int16_t *)tdc_buffer->ADD_output_va;

	for (i = 0; i < pixel_count; i++) {
		float correction =
			dcc_k_ptr[i] * diff_Temp_DCC +
			lc_k_ptr[i] * diff_Temp_LC;

		/*
		 * GFX applies output = K * (input - B) >> shift.
		 * TDC uses K=1 and shift=0, so B stores the positive
		 * temperature-induced ADC drift that must be removed.
		 */
		out_ptr[i] = tdc_clip_correction(correction);
	}

	hd_common_mem_flush_cache((void *)tdc_buffer->ADD_output_va,
				  correction_table_size);
	return HD_OK;
}


HD_RESULT tdc_apply(HD_VIDEO_FRAME *in_frame,
		    HD_VIDEO_FRAME *out_frame,
		    UINTPTR out_pa,
		    HD_COMMON_MEM_VB_BLK out_blk,
		    UINT32 raw_width,
		    UINT32 raw_height)
{
	VENDOR_GFX_GRPH_TRIGGER_PARAM param = {0};
	HD_RESULT ret;
	UINT32 raw_lineoffset;

	if (in_frame == NULL || out_frame == NULL ||
	    in_frame->phy_addr[0] == 0 || out_pa == 0 || out_blk == 0) {
		printf("TDC apply frame buffer is invalid\n");
		return HD_ERR_NG;
	}
	if (tdc_buffer->all_1_pa == 0 || tdc_buffer->ADD_output_pa == 0) {
		printf("TDC correction table is not ready\n");
		return HD_ERR_NG;
	}
	if (tdc_validate_image_layout() != 0) {
		return HD_ERR_NG;
	}
	if (raw_width != g_tdc_layout.raw_width ||
	    raw_height != g_tdc_layout.raw_height) {
		printf("TDC raw size must be %ux%u, got %ux%u\n",
		       g_tdc_layout.raw_width, g_tdc_layout.raw_height,
		       raw_width, raw_height);
		return HD_ERR_NG;
	}

	raw_lineoffset = raw_width * sizeof(UINT16);

	memcpy(out_frame, in_frame, sizeof(*out_frame));

	param.command = VENDOR_GRPH_CMD_Non_uni_correction;
	param.format = VENDOR_GFX_GRPH_FORMAT_16BITS;
	param.property[0].en = 1;
	param.property[0].id = VENDOR_GFX_GRPH_PROPERTY_ID_NORMAL;
	param.property[0].data.property = 0;
	param.images[0].img_id = VENDOR_GFX_GRPH_IMG_ID_A;
	param.images[0].dram_addr = in_frame->phy_addr[0];
	param.images[0].lineoffset = raw_lineoffset;
	param.images[0].width = raw_lineoffset;
	param.images[0].height = raw_height;
	param.images[1].img_id = VENDOR_GFX_GRPH_IMG_ID_B;
	param.images[1].dram_addr = tdc_buffer->all_1_pa;
	param.images[1].lineoffset = raw_lineoffset;
	param.images[2].img_id = VENDOR_GFX_GRPH_IMG_ID_C;
	param.images[2].dram_addr = out_pa;
	param.images[2].lineoffset = raw_lineoffset;
	param.images[3].img_id = VENDOR_GFX_GRPH_IMG_ID_D;
	param.images[3].dram_addr = tdc_buffer->ADD_output_pa;
	param.images[3].lineoffset = raw_lineoffset;

	ret = vendor_gfx_grph_trigger(1, &param);
	if (ret != HD_OK) {
		printf("TDC vendor_gfx_grph_trigger fail=%d\n", ret);
	}

	out_frame->phy_addr[0] = out_pa;
	out_frame->blk = out_blk;

	return ret;
}
 





































