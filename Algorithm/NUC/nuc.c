#include "nuc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__LINUX)
#include <fcntl.h>
#endif

#include "../../Config/app_config.h"
#include "../../Device/sensor_temp.h"
#include "vendor_gfx.h"

#define NUC_DIR "/mnt/sd/thermal_op"
#define NUC_SENSOR_TEMP_DIR "/mnt/sd/thermal_op/sensor_temp"
#define NUC_FIVE_POINT_COUNT 5
#define NUC_CALIBRATION_FRAME_COUNT 4U
#define NUC_SENSOR_TEMP_DEFAULT_INTERVAL_SEC 60U
#define NUC_TABLE_FILE_COUNT (NUC_FIVE_POINT_COUNT * 2U)
#define NUC_TABLE_STAGE_SUFFIX ".new"
#define NUC_TABLE_BACKUP_SUFFIX ".bak"
#define MENU_OPTION_REFRESH (-2)

typedef struct {
	UINTPTR k_va;
	UINTPTR b_va;
	UINT32 table_size;
	UINT32 raw_width;
	UINT32 raw_height;
	UINT32 active_x;
	UINT32 active_y;
	UINT32 active_width;
	UINT32 active_height;
	int enabled;
	int table_loaded;
	int sensor_temp_table_point;
	float sensor_temp_points[NUC_FIVE_POINT_COUNT];
	NUC_CAPTURE_RAW_CB capture_raw;
	NUC_CALIBRATION_STATE_CB calibration_state;
} NUC_CONTEXT;

typedef struct {
	char final_path[128];
	char stage_path[128];
	char backup_path[128];
	int had_original;
	int installed;
} NUC_TABLE_TRANSACTION_FILE;

static NUC_CONTEXT g_nuc;
static UINT32 g_nuc_sensor_temp_interval_sec =
	NUC_SENSOR_TEMP_DEFAULT_INTERVAL_SEC;

static void nuc_sync_sensor_temp_dir(void)
{
#if defined(__LINUX)
	int dir_fd = open(NUC_SENSOR_TEMP_DIR, O_RDONLY);

	if (dir_fd < 0) {
		printf("warning: open NUC table directory fail: %s\n",
		       strerror(errno));
		return;
	}
	if (fsync(dir_fd) != 0) {
		printf("warning: sync NUC table directory fail: %s\n",
		       strerror(errno));
	}
	close(dir_fd);
#endif
}

static int nuc_read_option(void)
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

static int nuc_read_file_to_mem(const char *path, void *addr, UINT32 size)
{
	FILE *fp;
	UINT32 read_len;

	fp = fopen(path, "rb");
	if (fp == 0) {
		printf("open file (%s) fail\n", path);
		return -1;
	}

	read_len = (UINT32)fread(addr, 1, size, fp);
	fclose(fp);

	if (read_len != size) {
		printf("read file (%s) len error, read=%u expect=%u\n",
		       path, read_len, size);
		return -1;
	}

	return 0;
}

static int nuc_write_mem_to_file(const char *path, const void *addr, UINT32 size)
{
	FILE *fp = NULL;
	UINT32 write_len;
	int saved_errno;

	fp = fopen(path, "wb");
	if (fp == 0) {
		printf("open file (%s) fail\n", path);
		return -1;
	}

	write_len = (UINT32)fwrite(addr, 1, size, fp);
	if (write_len != size || ferror(fp)) {
		printf("write file (%s) len error, write=%u expect=%u\n",
		       path, write_len, size);
		goto fail;
	}
	if (fflush(fp) != 0) {
		printf("flush file (%s) fail: %s\n", path, strerror(errno));
		goto fail;
	}
#if defined(__LINUX)
	if (fsync(fileno(fp)) != 0) {
		printf("sync file (%s) fail: %s\n", path, strerror(errno));
		goto fail;
	}
#endif
	if (fclose(fp) != 0) {
		fp = NULL;
		printf("close file (%s) fail: %s\n", path, strerror(errno));
		goto fail;
	}
	fp = NULL;

	return 0;

fail:
	saved_errno = errno;
	if (fp != NULL) {
		fclose(fp);
	}
	if (remove(path) != 0 && errno != ENOENT) {
		printf("remove incomplete file (%s) fail: %s\n",
		       path, strerror(errno));
	}
	errno = saved_errno;
	return -1;
}

static int nuc_validate_table_file(const char *path)
{
	struct stat file_stat;

	if (stat(path, &file_stat) != 0) {
		printf("NUC table file missing: %s\n", path);
		return -1;
	}
	if ((UINT64)file_stat.st_size != g_nuc.table_size) {
		printf("NUC table size error: %s, size=%lld, expect=%u\n",
		       path, (long long)file_stat.st_size, g_nuc.table_size);
		return -1;
	}

	return 0;
}

static void nuc_format_sensor_temp_table_path(char *path, size_t path_size,
					      UINT32 point, const char *type,
					      const char *suffix)
{
	snprintf(path, path_size, "%s/point%u_%s.bin%s",
		 NUC_SENSOR_TEMP_DIR, point + 1, type, suffix);
}

static void nuc_prepare_table_transaction(NUC_TABLE_TRANSACTION_FILE *files)
{
	UINT32 point;

	memset(files, 0,
	       NUC_TABLE_FILE_COUNT * sizeof(NUC_TABLE_TRANSACTION_FILE));
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		UINT32 k_index = point * 2U;
		UINT32 b_index = k_index + 1U;

		nuc_format_sensor_temp_table_path(
			files[k_index].final_path,
			sizeof(files[k_index].final_path), point, "K", "");
		nuc_format_sensor_temp_table_path(
			files[k_index].stage_path,
			sizeof(files[k_index].stage_path), point, "K",
			NUC_TABLE_STAGE_SUFFIX);
		nuc_format_sensor_temp_table_path(
			files[k_index].backup_path,
			sizeof(files[k_index].backup_path), point, "K",
			NUC_TABLE_BACKUP_SUFFIX);

		nuc_format_sensor_temp_table_path(
			files[b_index].final_path,
			sizeof(files[b_index].final_path), point, "B", "");
		nuc_format_sensor_temp_table_path(
			files[b_index].stage_path,
			sizeof(files[b_index].stage_path), point, "B",
			NUC_TABLE_STAGE_SUFFIX);
		nuc_format_sensor_temp_table_path(
			files[b_index].backup_path,
			sizeof(files[b_index].backup_path), point, "B",
			NUC_TABLE_BACKUP_SUFFIX);
	}
}

static int nuc_cleanup_staged_sensor_temp_tables(void)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_TABLE_FILE_COUNT];
	UINT32 i;
	int ret = 0;
	int changed = 0;

	nuc_prepare_table_transaction(files);
	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (remove(files[i].stage_path) == 0) {
			changed = 1;
		} else if (errno != ENOENT) {
			printf("remove staged NUC table fail: %s\n",
			       files[i].stage_path);
			ret = -1;
		}
	}
	if (changed) {
		nuc_sync_sensor_temp_dir();
	}

	return ret;
}

static int nuc_restore_table_backup(
	const NUC_TABLE_TRANSACTION_FILE *file)
{
	void *table = NULL;
	int ret = -1;

	table = malloc(g_nuc.table_size);
	if (table == NULL) {
		printf("allocate NUC table recovery buffer fail\n");
		return -1;
	}
	if (nuc_read_file_to_mem(file->backup_path, table,
				 g_nuc.table_size) != 0) {
		goto exit;
	}
	if (nuc_write_mem_to_file(file->stage_path, table,
				  g_nuc.table_size) != 0) {
		goto exit;
	}
	if (rename(file->stage_path, file->final_path) != 0) {
		printf("restore NUC table backup fail: %s, %s\n",
		       file->final_path, strerror(errno));
		goto exit;
	}

	ret = 0;

exit:
	free(table);
	return ret;
}

static int nuc_recover_sensor_temp_tables(void)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_TABLE_FILE_COUNT];
	int stage_exists[NUC_TABLE_FILE_COUNT] = {0};
	int backup_exists[NUC_TABLE_FILE_COUNT] = {0};
	UINT32 i;
	int stage_found = 0;
	int backup_found = 0;
	int interrupted = 0;
	int changed = 0;
	int ret = 0;

	if (access(NUC_SENSOR_TEMP_DIR, F_OK) != 0) {
		if (errno == ENOENT) {
			return 0;
		}
		printf("check NUC table directory fail: %s\n", strerror(errno));
		return -1;
	}

	nuc_prepare_table_transaction(files);
	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (access(files[i].stage_path, F_OK) == 0) {
			stage_exists[i] = 1;
			stage_found = 1;
		} else if (errno != ENOENT) {
			printf("check staged NUC table fail: %s\n",
			       files[i].stage_path);
			return -1;
		}
		if (access(files[i].backup_path, F_OK) == 0) {
			backup_exists[i] = 1;
			backup_found = 1;
		} else if (errno != ENOENT) {
			printf("check NUC table backup fail: %s\n",
			       files[i].backup_path);
			return -1;
		}
	}
	interrupted = stage_found && backup_found;

	if (interrupted) {
		printf("Interrupted NUC table update found, restoring previous tables\n");
		for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
			if (backup_exists[i]) {
				if (nuc_restore_table_backup(&files[i]) != 0) {
					ret = -1;
				}
				changed = 1;
			} else if (!stage_exists[i]) {
				/* Installed during the interrupted update without an old file. */
				if (remove(files[i].final_path) == 0) {
					changed = 1;
				} else if (errno != ENOENT) {
					printf("remove incomplete NUC table fail: %s\n",
					       files[i].final_path);
					ret = -1;
				}
			}
		}
		if (ret != 0) {
			if (changed) {
				nuc_sync_sensor_temp_dir();
			}
			return -1;
		}
	} else if (stage_found) {
		/* Calibration stopped before any final table was backed up. */
		printf("Incomplete staged NUC tables found, discarding stages\n");
	}

	if (nuc_cleanup_staged_sensor_temp_tables() != 0) {
		ret = -1;
	}
	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (!backup_exists[i]) {
			continue;
		}

		if (!interrupted && access(files[i].final_path, F_OK) != 0) {
			if (errno != ENOENT ||
			    nuc_restore_table_backup(&files[i]) != 0) {
				ret = -1;
				continue;
			}
		}
		if (remove(files[i].backup_path) == 0) {
			changed = 1;
		} else if (errno != ENOENT) {
			printf("remove stale NUC table backup fail: %s\n",
			       files[i].backup_path);
			ret = -1;
		}
	}
	if (changed) {
		nuc_sync_sensor_temp_dir();
	}
	if (interrupted && ret == 0) {
		printf("Previous NUC Sensor Temperature tables restored\n");
	}

	return ret;
}

static int nuc_validate_all_sensor_temp_tables(void)
{
	char k_path[128];
	char b_path[128];
	UINT32 point;

	if (g_nuc.table_size == 0) {
		printf("NUC table size is not ready\n");
		return -1;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		snprintf(k_path, sizeof(k_path), "%s/point%u_K.bin",
			 NUC_SENSOR_TEMP_DIR, point + 1);
		snprintf(b_path, sizeof(b_path), "%s/point%u_B.bin",
			 NUC_SENSOR_TEMP_DIR, point + 1);

		if (nuc_validate_table_file(k_path) != 0 ||
		    nuc_validate_table_file(b_path) != 0) {
			printf("NUC Sensor Temperature point %u table is incomplete\n",
			       point + 1);
			return -1;
		}
	}

	printf("All %u NUC Sensor Temperature K/B tables are valid\n",
	       NUC_FIVE_POINT_COUNT);
	return 0;
}

static int nuc_wait_enter(const char *name)
{
	char buf[16];

	printf("\n%s blackbody calibration.\n", name);
	printf("Put sensor toward %s blackbody, then press Enter.\n", name);
	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == 0) {
		return -1;
	}

	return 0;
}

static UINT16 nuc_clip_u16(INT32 value)
{
	if (value < 0) {
		return 0;
	}
	if (value > 65535) {
		return 65535;
	}
	return (UINT16)value;
}

static UINT16 nuc_store_b_i64(long long value)
{
	if (value < -32768) {
		value = -32768;
	} else if (value > 32767) {
		value = 32767;
	}

	return (UINT16)(short)value;
}

static long long nuc_div_round_nearest(long long numerator, long long denominator)
{
	if (numerator >= 0) {
		return (numerator + denominator / 2) / denominator;
	}

	return -((-numerator + denominator / 2) / denominator);
}

static UINT16 nuc_convert_add_b_to_gfx_b(long long add_b, UINT16 k_q)
{
	long long numerator;
	long long gfx_b;

	/* Convert y=(K*x>>n)+add_b to GFX y=K*(x-gfx_b)>>n. */
	numerator = -(add_b * (long long)NUC_GAIN_ONE);
	gfx_b = nuc_div_round_nearest(numerator, k_q);
	return nuc_store_b_i64(gfx_b);
}

static int nuc_make_output_dir(void)
{
	if (mkdir(NUC_DIR, 0755) != 0 && access(NUC_DIR, F_OK) != 0) {
		printf("create dir (%s) fail\n", NUC_DIR);
		return -1;
	}

	return 0;
}

static int nuc_make_sensor_temp_dir(void)
{
	if (nuc_make_output_dir() != 0) {
		return -1;
	}
	if (mkdir(NUC_SENSOR_TEMP_DIR, 0755) != 0 &&
	    access(NUC_SENSOR_TEMP_DIR, F_OK) != 0) {
		printf("create dir (%s) fail\n", NUC_SENSOR_TEMP_DIR);
		return -1;
	}

	return 0;
}

static int nuc_wait_sensor_temp_point(UINT32 point)
{
	char buf[16];

	printf("\nSensor temperature calibration point %u/%u.\n",
	       point + 1, NUC_FIVE_POINT_COUNT);
	printf("Set and stabilize the sensor temperature, then press Enter.\n");
	printf(">> ");
	if (fgets(buf, sizeof(buf), stdin) == 0) {
		return -1;
	}

	return 0;
}

static int nuc_validate_image_layout(void)
{
	if (g_nuc.raw_width == 0 || g_nuc.raw_height == 0 ||
	    g_nuc.active_width == 0 || g_nuc.active_height == 0 ||
	    g_nuc.active_x >= g_nuc.raw_width ||
	    g_nuc.active_y >= g_nuc.raw_height ||
	    g_nuc.active_width > g_nuc.raw_width - g_nuc.active_x ||
	    g_nuc.active_height > g_nuc.raw_height - g_nuc.active_y ||
	    (UINT64)g_nuc.raw_width * g_nuc.raw_height * sizeof(UINT16) !=
		g_nuc.table_size) {
		printf("NUC image layout is invalid\n");
		return -1;
	}

	return 0;
}

static int nuc_build_table(const UINT16 *low_raw,
			   const UINT16 *high_raw,
			   UINT16 *k_tbl,
			   UINT16 *b_tbl)
{
	UINT64 low_sum = 0;
	UINT64 high_sum = 0;
	UINT32 low_avg;
	UINT32 high_avg;
	UINT32 raw_pixel_count;
	UINT32 active_pixel_count;
	UINT32 x;
	UINT32 y;
	INT32 target_delta;

	if (nuc_validate_image_layout() != 0) {
		return -1;
	}

	raw_pixel_count = g_nuc.raw_width * g_nuc.raw_height;
	active_pixel_count = g_nuc.active_width * g_nuc.active_height;

	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;

		for (x = 0; x < g_nuc.active_width; x++) {
			low_sum += low_raw[row + x];
			high_sum += high_raw[row + x];
		}
	}

	low_avg = (UINT32)(low_sum / active_pixel_count);
	high_avg = (UINT32)(high_sum / active_pixel_count);
	target_delta = (INT32)high_avg - (INT32)low_avg;
	printf("NUC active %ux%u low_avg=%u, high_avg=%u\n",
	       g_nuc.active_width, g_nuc.active_height, low_avg, high_avg);

	/* Reject the whole calibration point before changing the current table. */
	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;

		for (x = 0; x < g_nuc.active_width; x++) {
			UINT32 pixel = row + x;
			INT32 delta = (INT32)high_raw[pixel] -
				(INT32)low_raw[pixel];

			if (delta == 0 || target_delta == 0) {
				continue;
			}

			if ((target_delta < 0) != (delta < 0)) {
				printf("NUC invalid negative K at raw (%u,%u): low=%u, high=%u, target_delta=%d, pixel_delta=%d\n",
				       g_nuc.active_x + x, g_nuc.active_y + y,
				       low_raw[pixel], high_raw[pixel],
				       target_delta, delta);
				return -1;
			}
		}
	}

	/* Keep dummy pixels unchanged when GFX processes the full RAW frame. */
	for (x = 0; x < raw_pixel_count; x++) {
		k_tbl[x] = (UINT16)NUC_GAIN_ONE;
		b_tbl[x] = 0;
	}

	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;

		for (x = 0; x < g_nuc.active_width; x++) {
			UINT32 pixel = row + x;
			INT32 delta = (INT32)high_raw[pixel] -
				(INT32)low_raw[pixel];
			INT32 k_q;
			long long add_b;

			if (delta == 0 || target_delta == 0) {
				continue;
			}

			k_q = (INT32)(((long long)target_delta *
				NUC_GAIN_ONE) / delta);
			k_tbl[pixel] = nuc_clip_u16(k_q);
			if (k_tbl[pixel] == 0) {
				k_tbl[pixel] = (UINT16)NUC_GAIN_ONE;
				continue;
			}

			add_b = (long long)low_avg -
				(((long long)k_tbl[pixel] * low_raw[pixel]) >>
				 NUC_GAIN_SHIFT);
			b_tbl[pixel] = nuc_convert_add_b_to_gfx_b(
				add_b, k_tbl[pixel]);
		}
	}

	return 0;
}

static int nuc_stage_sensor_temp_table(UINT32 point)
{
	char k_path[128];
	char b_path[128];

	if (nuc_make_sensor_temp_dir() != 0) {
		return -1;
	}

	nuc_format_sensor_temp_table_path(k_path, sizeof(k_path), point, "K",
					  NUC_TABLE_STAGE_SUFFIX);
	nuc_format_sensor_temp_table_path(b_path, sizeof(b_path), point, "B",
					  NUC_TABLE_STAGE_SUFFIX);

	hd_common_mem_flush_cache((void *)g_nuc.k_va, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.b_va, g_nuc.table_size);

	if (nuc_write_mem_to_file(k_path, (void *)g_nuc.k_va,
				  g_nuc.table_size) != 0) {
		return -1;
	}
	if (nuc_write_mem_to_file(b_path, (void *)g_nuc.b_va,
				  g_nuc.table_size) != 0) {
		return -1;
	}
	nuc_sync_sensor_temp_dir();

	printf("Sensor temperature point %u K/B table staged\n", point + 1);
	return 0;
}

static int nuc_commit_staged_sensor_temp_tables(const float *temperatures)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_TABLE_FILE_COUNT];
	UINT32 i;
	UINT32 point;
	int ret = -1;

	nuc_prepare_table_transaction(files);
	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (nuc_validate_table_file(files[i].stage_path) != 0) {
			printf("NUC staged table validation failed\n");
			goto rollback;
		}
	}
	nuc_sync_sensor_temp_dir();

	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (access(files[i].final_path, F_OK) == 0) {
			if (rename(files[i].final_path,
				   files[i].backup_path) != 0) {
				printf("backup NUC table fail: %s\n",
				       files[i].final_path);
				goto rollback;
			}
			files[i].had_original = 1;
			nuc_sync_sensor_temp_dir();
		} else if (errno != ENOENT) {
			printf("check NUC table fail: %s\n",
			       files[i].final_path);
			goto rollback;
		}

		if (rename(files[i].stage_path, files[i].final_path) != 0) {
			printf("install staged NUC table fail: %s\n",
			       files[i].final_path);
			goto rollback;
		}
		files[i].installed = 1;
		nuc_sync_sensor_temp_dir();
	}

	if (app_config_update_sensor_temp_points(
		    temperatures, NUC_FIVE_POINT_COUNT) != 0) {
		printf("save Sensor Temperature points to tiffany_thermal.cfg fail\n");
		goto rollback;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		g_nuc.sensor_temp_points[point] = temperatures[point];
	}
	for (i = 0; i < NUC_TABLE_FILE_COUNT; i++) {
		if (files[i].had_original &&
		    remove(files[i].backup_path) != 0 && errno != ENOENT) {
			printf("remove old NUC table backup fail: %s\n",
			       files[i].backup_path);
		}
	}
	nuc_sync_sensor_temp_dir();

	printf("All NUC Sensor Temperature K/B tables committed\n");
	return 0;

rollback:
	for (i = NUC_TABLE_FILE_COUNT; i > 0; i--) {
		NUC_TABLE_TRANSACTION_FILE *file = &files[i - 1U];

		if (file->installed &&
		    remove(file->final_path) != 0 && errno != ENOENT) {
			printf("remove new NUC table during rollback fail: %s\n",
			       file->final_path);
		}
		if (file->had_original &&
		    rename(file->backup_path, file->final_path) != 0) {
			printf("restore old NUC table fail: %s\n",
			       file->final_path);
		}
		nuc_sync_sensor_temp_dir();
	}
	nuc_cleanup_staged_sensor_temp_tables();
	nuc_sync_sensor_temp_dir();
	return ret;
}

static int nuc_capture_average_raw(const char *name,
				   UINT16 *average_raw,
				   UINT16 *frame_raw,
				   UINT32 *raw_sum)
{
	UINT32 active_pixel_count;
	UINT32 frame;
	UINT32 x;
	UINT32 y;

	if (name == NULL || average_raw == NULL || frame_raw == NULL ||
	    raw_sum == NULL || g_nuc.capture_raw == NULL ||
	    nuc_validate_image_layout() != 0) {
		return -1;
	}
	if (nuc_wait_enter(name) != 0) {
		return -1;
	}

	active_pixel_count = g_nuc.active_width * g_nuc.active_height;
	memset(raw_sum, 0, active_pixel_count * sizeof(*raw_sum));

	for (frame = 0; frame < NUC_CALIBRATION_FRAME_COUNT; frame++) {
		if (g_nuc.capture_raw((UINTPTR)frame_raw,
				      g_nuc.table_size) != 0) {
			printf("capture %s RAW frame %u/%u fail\n", name,
			       frame + 1, NUC_CALIBRATION_FRAME_COUNT);
			return -1;
		}

		for (y = 0; y < g_nuc.active_height; y++) {
			UINT32 raw_row = (g_nuc.active_y + y) *
				g_nuc.raw_width + g_nuc.active_x;
			UINT32 sum_row = y * g_nuc.active_width;

			for (x = 0; x < g_nuc.active_width; x++) {
				raw_sum[sum_row + x] += frame_raw[raw_row + x];
			}
		}

		printf("NUC %s average frame %u/%u\n", name,
		       frame + 1, NUC_CALIBRATION_FRAME_COUNT);
	}

	memset(average_raw, 0, g_nuc.table_size);
	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 raw_row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;
		UINT32 sum_row = y * g_nuc.active_width;

		for (x = 0; x < g_nuc.active_width; x++) {
			average_raw[raw_row + x] = (UINT16)(
				(raw_sum[sum_row + x] +
				 NUC_CALIBRATION_FRAME_COUNT / 2U) /
				NUC_CALIBRATION_FRAME_COUNT);
		}
	}

	return 0;
}

static int nuc_capture_two_point(UINT16 *low_raw, UINT16 *high_raw,
				 UINT16 *frame_raw, UINT32 *raw_sum)
{
	if (nuc_capture_average_raw("Low temperature", low_raw,
				    frame_raw, raw_sum) != 0) {
		return -1;
	}
	if (nuc_capture_average_raw("High temperature", high_raw,
				    frame_raw, raw_sum) != 0) {
		return -1;
	}

	return 0;
}

void nuc_bind_runtime(UINTPTR k_va, UINTPTR b_va, UINT32 table_size,
		      const NUC_IMAGE_LAYOUT *layout)
{
	g_nuc.k_va = k_va;
	g_nuc.b_va = b_va;
	g_nuc.table_size = table_size;
	if (layout != NULL) {
		g_nuc.raw_width = layout->raw_width;
		g_nuc.raw_height = layout->raw_height;
		g_nuc.active_x = layout->active_x;
		g_nuc.active_y = layout->active_y;
		g_nuc.active_width = layout->active_width;
		g_nuc.active_height = layout->active_height;
	} else {
		g_nuc.raw_width = 0;
		g_nuc.raw_height = 0;
		g_nuc.active_x = 0;
		g_nuc.active_y = 0;
		g_nuc.active_width = 0;
		g_nuc.active_height = 0;
	}
	g_nuc.sensor_temp_table_point = -1;
}

void nuc_bind_capture_callback(NUC_CAPTURE_RAW_CB capture_raw)
{
	g_nuc.capture_raw = capture_raw;
}

void nuc_bind_calibration_state_callback(
	NUC_CALIBRATION_STATE_CB calibration_state)
{
	g_nuc.calibration_state = calibration_state;
}

void nuc_set_sensor_temp_point(UINT32 point, float temp_c)
{
	if (point < NUC_FIVE_POINT_COUNT) {
		g_nuc.sensor_temp_points[point] = temp_c;
	}
}

float nuc_get_sensor_temp_point(UINT32 point)
{
	if (point >= NUC_FIVE_POINT_COUNT) {
		return 0.0f;
	}

	return g_nuc.sensor_temp_points[point];
}

static int nuc_load_sensor_temp_table(UINT32 point)
{
	char k_path[128];
	char b_path[128];
	UINT16 *k_table = 0;
	UINT16 *b_table = 0;
	int ret = -1;

	if (point >= NUC_FIVE_POINT_COUNT) {
		printf("invalid NUC Sensor Temperature table point %u\n",
		       point + 1);
		return -1;
	}
	if (g_nuc.table_size == 0) {
		printf("NUC table size is not ready\n");
		return -1;
	}

	if (g_nuc.k_va == 0) {
		printf("NUC K table buffer is not ready\n");
		return -1;
	}

	if (g_nuc.b_va == 0) {
		printf("NUC B table buffer is not ready\n");
		return -1;
	}

	snprintf(k_path, sizeof(k_path), "%s/point%u_K.bin",
		 NUC_SENSOR_TEMP_DIR, point + 1);
	snprintf(b_path, sizeof(b_path), "%s/point%u_B.bin",
		 NUC_SENSOR_TEMP_DIR, point + 1);
	if (nuc_validate_table_file(k_path) != 0 ||
	    nuc_validate_table_file(b_path) != 0) {
		return -1;
	}

	k_table = (UINT16 *)malloc(g_nuc.table_size);
	b_table = (UINT16 *)malloc(g_nuc.table_size);
	if (k_table == 0 || b_table == 0) {
		printf("allocate NUC Sensor Temperature table fail\n");
		goto exit;
	}

	if (nuc_read_file_to_mem(k_path, k_table, g_nuc.table_size) != 0 ||
	    nuc_read_file_to_mem(b_path, b_table, g_nuc.table_size) != 0) {
		goto exit;
	}

	memcpy((void *)g_nuc.k_va, k_table, g_nuc.table_size);
	memcpy((void *)g_nuc.b_va, b_table, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.k_va, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.b_va, g_nuc.table_size);

	g_nuc.table_loaded = 1;
	g_nuc.sensor_temp_table_point = (int)point;
	printf("NUC Sensor Temperature point %u K/B table loaded\n",
	       point + 1);
	ret = 0;

exit:
	free(k_table);
	free(b_table);
	return ret;
}

static int nuc_sensor_temp_points_are_ready(void)
{
	UINT32 point;

	for (point = 1; point < NUC_FIVE_POINT_COUNT; point++) {
		if (g_nuc.sensor_temp_points[point] <=
		    g_nuc.sensor_temp_points[point - 1]) {
			return 0;
		}
	}

	return 1;
}

int nuc_reload_table(void)
{
	int ret;

	if (nuc_validate_all_sensor_temp_tables() != 0) {
		goto fail;
	}

	if (!nuc_sensor_temp_points_are_ready()) {
		printf("NUC Sensor Temperature points are not ready, load point 1\n");
		ret = nuc_load_sensor_temp_table(0);
	} else {
		g_nuc.sensor_temp_table_point = -1;
		ret = nuc_update_sensor_temp_table();
	}

	if (ret != 0) {
		goto fail;
	}
	return 0;

fail:
	g_nuc.table_loaded = 0;
	nuc_set_enabled(0);
	return -1;
}

int nuc_update_sensor_temp_table(void)
{
	float temp_c;
	UINT32 point;

	if (!nuc_sensor_temp_points_are_ready()) {
		printf("NUC Sensor Temperature points are not ready\n");
		return -1;
	}

	if (sensor_temp_read(&temp_c) != 0) {
		printf("read runtime sensor temperature fail\n");
		return -1;
	}

	point = 0;
	while (point + 1 < NUC_FIVE_POINT_COUNT) {
		float boundary =
			(g_nuc.sensor_temp_points[point] +
			 g_nuc.sensor_temp_points[point + 1]) * 0.5f;

		if (temp_c < boundary) {
			break;
		}
		point++;
	}

	if ((int)point == g_nuc.sensor_temp_table_point) {
		return 0;
	}

	if (nuc_load_sensor_temp_table(point) != 0) {
		g_nuc.table_loaded = 0;
		nuc_set_enabled(0);
		return -1;
	}

	printf("NUC Sensor Temperature %.4f C, use point %u (%.4f C)\n",
	       temp_c, point + 1, g_nuc.sensor_temp_points[point]);
	return 0;
}

int nuc_five_point_sensor_temp_calibration(void)
{
	UINT16 *low_raw = 0;
	UINT16 *high_raw = 0;
	UINT16 *frame_raw = 0;
	UINT32 *raw_sum = 0;
	NUC_CALIBRATION_STATE_CB calibration_state = 0;
	float temperatures[NUC_FIVE_POINT_COUNT];
	UINT32 active_pixel_count;
	UINT32 point;
	int ret = -1;

	if (g_nuc.table_size == 0 || g_nuc.k_va == 0 || g_nuc.b_va == 0) {
		printf("NUC runtime buffer is not ready\n");
		return -1;
	}
	if (g_nuc.capture_raw == 0) {
		printf("NUC capture callback is not ready\n");
		return -1;
	}
	if (nuc_validate_image_layout() != 0) {
		return -1;
	}
	if (nuc_cleanup_staged_sensor_temp_tables() != 0) {
		printf("clean old staged NUC tables fail\n");
		return -1;
	}

	calibration_state = g_nuc.calibration_state;
	if (calibration_state != 0) {
		calibration_state("NUC", 1);
	}

	active_pixel_count = g_nuc.active_width * g_nuc.active_height;
	low_raw = (UINT16 *)malloc(g_nuc.table_size);
	high_raw = (UINT16 *)malloc(g_nuc.table_size);
	frame_raw = (UINT16 *)malloc(g_nuc.table_size);
	raw_sum = (UINT32 *)malloc(active_pixel_count * sizeof(*raw_sum));
	if (low_raw == 0 || high_raw == 0 || frame_raw == 0 || raw_sum == 0) {
		printf("5 point Sensor Temperature NUC calibration malloc fail\n");
		goto exit;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		if (nuc_wait_sensor_temp_point(point) != 0) {
			goto exit;
		}
		if (sensor_temp_read(&temperatures[point]) != 0) {
			printf("read sensor temperature point %u fail\n", point + 1);
			goto exit;
		}
		printf("Sensor temperature point %u = %.4f C\n",
		       point + 1, temperatures[point]);

		if (nuc_capture_two_point(low_raw, high_raw,
					  frame_raw, raw_sum) != 0) {
			printf("capture sensor temperature point %u fail\n", point + 1);
			goto exit;
		}

		if (nuc_build_table(low_raw, high_raw,
				    (UINT16 *)g_nuc.k_va,
				    (UINT16 *)g_nuc.b_va) != 0) {
			printf("NUC calibration point %u rejected; abort calibration\n",
			       point + 1);
			goto exit;
		}
		if (nuc_stage_sensor_temp_table(point) != 0) {
			goto exit;
		}
	}

	if (nuc_commit_staged_sensor_temp_tables(temperatures) != 0) {
		printf("commit Sensor Temperature NUC tables fail\n");
		goto exit;
	}

	printf("5 point Sensor Temperature NUC calibration done\n");
	ret = 0;

exit:
	if (ret != 0) {
		nuc_cleanup_staged_sensor_temp_tables();
	}
	free(low_raw);
	free(high_raw);
	free(frame_raw);
	free(raw_sum);

	nuc_reload_table();
	if (calibration_state != 0) {
		calibration_state("NUC", 0);
	}

	return ret;
}

int nuc_init_table(void)
{
	if (nuc_recover_sensor_temp_tables() != 0) {
		printf("NUC table recovery failed, disable NUC\n");
		return -1;
	}
	if (nuc_reload_table() != 0) {
		printf("NUC table not found or invalid, disable NUC\n");
		return -1;
	}

	nuc_set_enabled(1);
	return 0;
}

void nuc_set_enabled(int enable)
{
	if (enable && !g_nuc.table_loaded) {
		printf("NUC table not loaded, keep NUC off\n");
		g_nuc.enabled = 0;
		printf("NUC off\n");
		return;
	}

	g_nuc.enabled = enable ? 1 : 0;
	printf("NUC %s\n", g_nuc.enabled ? "on" : "off");
}

int nuc_is_enabled(void)
{
	return g_nuc.enabled;
}

void nuc_set_sensor_temp_interval_sec(UINT32 interval_sec)
{
	if (interval_sec == 0) {
		printf("NUC Sensor Temperature interval must be greater than 0 sec\n");
		return;
	}

	g_nuc_sensor_temp_interval_sec = interval_sec;
	printf("NUC Sensor Temperature interval=%u sec\n",
	       g_nuc_sensor_temp_interval_sec);
}

UINT32 nuc_get_sensor_temp_interval_sec(void)
{
	return g_nuc_sensor_temp_interval_sec;
}

int nuc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("NUC menu\n");
		printf(" 1. 5 point Sensor Temperature NUC calibration\n");
		printf(" 2. Reload Sensor Temperature K/B table\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: NUC=%s, Sensor Temperature interval=%u sec\n",
		       g_nuc.enabled ? "on" : "off",
		       g_nuc_sensor_temp_interval_sec);
		printf("Settings: CFG Setting -> NUC setting\n");

		option = nuc_read_option();
		switch (option) {
		case 1:
			ret = nuc_five_point_sensor_temp_calibration();
			break;

		case 2:
			ret = nuc_reload_table();
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

HD_RESULT nuc_apply(HD_VIDEO_FRAME *in_frame,
		    HD_VIDEO_FRAME *out_frame,
		    UINTPTR k_frame_pa,
		    UINTPTR b_frame_pa,
		    UINTPTR out_pa,
		    HD_COMMON_MEM_VB_BLK out_blk,
		    UINT32 raw_width,
		    UINT32 raw_height)
{
	VENDOR_GFX_GRPH_TRIGGER_PARAM param = {0};
	HD_RESULT ret;

	memcpy(out_frame, in_frame, sizeof(HD_VIDEO_FRAME));

	param.command = VENDOR_GRPH_CMD_Non_uni_correction;
	param.format = VENDOR_GFX_GRPH_FORMAT_16BITS;
	param.property[0].en = 1;
	param.property[0].id = VENDOR_GFX_GRPH_PROPERTY_ID_NORMAL;
	param.property[0].data.property = NUC_GAIN_SHIFT;

	param.images[0].img_id = VENDOR_GFX_GRPH_IMG_ID_A;
	param.images[0].dram_addr = in_frame->phy_addr[0];
	param.images[0].lineoffset = raw_width * 2;
	param.images[0].width = raw_width * 2;
	param.images[0].height = raw_height;

	param.images[1].img_id = VENDOR_GFX_GRPH_IMG_ID_B;
	param.images[1].dram_addr = k_frame_pa;
	param.images[1].lineoffset = raw_width * 2;

	param.images[2].img_id = VENDOR_GFX_GRPH_IMG_ID_C;
	param.images[2].dram_addr = out_pa;
	param.images[2].lineoffset = raw_width * 2;

	param.images[3].img_id = VENDOR_GFX_GRPH_IMG_ID_D;
	param.images[3].dram_addr = b_frame_pa;
	param.images[3].lineoffset = raw_width * 2;

	ret = vendor_gfx_grph_trigger(1, &param);
	if (ret != HD_OK) {
		printf("vendor_gfx_grph_trigger fail=%d\n", ret);
	}

	out_frame->phy_addr[0] = out_pa;
	out_frame->blk = out_blk;

	return ret;
}
