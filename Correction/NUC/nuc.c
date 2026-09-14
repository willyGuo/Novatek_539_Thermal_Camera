#include "nuc.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#if defined(__LINUX)
#include <fcntl.h>
#endif

#include "../../Config/app_config.h"
#include "../BPR/bpr.h"
#include "../../Device/SensorTemp/sensor_temp.h"
#include "vendor_gfx.h"

#define NUC_DIR "/mnt/sd/thermal_op"
#define NUC_SENSOR_TEMP_DIR "/mnt/sd/thermal_op/sensor_temp"
#define NUC_BLACKBODY_TEMP_DIR "/mnt/sd/thermal_op/blackbody_temp"
#define NUC_FIVE_POINT_COUNT 5
#define NUC_CALIBRATION_FRAME_COUNT 4U
#define NUC_SENSOR_TEMP_DEFAULT_INTERVAL_SEC 60U
#define NUC_SENSOR_TABLE_FILE_COUNT (NUC_FIVE_POINT_COUNT * 2U)
#define NUC_BLACKBODY_TABLE_FILE_COUNT (NUC_FIVE_POINT_COUNT * 3U)
#define NUC_DYNAMIC_BOUNDARY_COUNT (NUC_FIVE_POINT_COUNT - 1U)
#define NUC_DYNAMIC_REPORT_FRAME_COUNT 60U
/* Temporary: allow invalid reference ordering so board performance can be measured. */
#define NUC_DYNAMIC_PERF_ALLOW_NON_MONOTONIC 1
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
	int blackbody_dynamic_enabled;
	UINT16 *blackbody_source_tables;
	UINT16 *blackbody_k[NUC_FIVE_POINT_COUNT];
	UINT16 *blackbody_b[NUC_FIVE_POINT_COUNT];
	UINT16 *blackbody_thresholds;
	UINT32 blackbody_reference_mean[NUC_FIVE_POINT_COUNT];
	UINT32 blackbody_invalid_pixel_count;
	UINT64 dynamic_report_frames;
	UINT64 dynamic_map_us;
	UINT64 dynamic_select_us;
	UINT64 dynamic_flush_us;
	UINT64 dynamic_gfx_us;
	UINT64 dynamic_max_total_us;
	UINT64 dynamic_point_pixels[NUC_FIVE_POINT_COUNT];
	UINT64 dynamic_last_frame_start_us;
	UINT64 dynamic_interval_us;
	UINT64 dynamic_interval_count;
	UINT64 dynamic_pending_prepare_us;
	int dynamic_perf_pending;
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

static void nuc_sync_table_dir(const char *dir)
{
#if defined(__LINUX)
	int dir_fd = open(dir, O_RDONLY);

	if (dir_fd < 0) {
		printf("warning: open NUC table directory (%s) fail: %s\n",
		       dir, strerror(errno));
		return;
	}
	if (fsync(dir_fd) != 0) {
		printf("warning: sync NUC table directory (%s) fail: %s\n",
		       dir, strerror(errno));
	}
	close(dir_fd);
#else
	(void)dir;
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

static int nuc_read_file_to_mem_internal(const char *path, void *addr,
					 UINT32 size, int verbose)
{
	FILE *fp;
	UINT32 read_len;

	fp = fopen(path, "rb");
	if (fp == 0) {
		if (verbose) {
			printf("open file (%s) fail\n", path);
		}
		return -1;
	}

	read_len = (UINT32)fread(addr, 1, size, fp);
	fclose(fp);

	if (read_len != size) {
		if (verbose) {
			printf("read file (%s) len error, read=%u expect=%u\n",
			       path, read_len, size);
		}
		return -1;
	}

	return 0;
}

static int nuc_read_file_to_mem(const char *path, void *addr, UINT32 size)
{
	return nuc_read_file_to_mem_internal(path, addr, size, 1);
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

static int nuc_validate_table_file_internal(const char *path, int verbose)
{
	struct stat file_stat;

	if (stat(path, &file_stat) != 0) {
		if (verbose) {
			printf("NUC table file missing: %s\n", path);
		}
		return -1;
	}
	if ((UINT64)file_stat.st_size != g_nuc.table_size) {
		if (verbose) {
			printf("NUC table size error: %s, size=%lld, expect=%u\n",
			       path, (long long)file_stat.st_size,
			       g_nuc.table_size);
		}
		return -1;
	}

	return 0;
}

static int nuc_validate_table_file(const char *path)
{
	return nuc_validate_table_file_internal(path, 1);
}

static void nuc_format_table_path(char *path, size_t path_size,
				  const char *dir, UINT32 point,
				  const char *type, const char *suffix)
{
	snprintf(path, path_size, "%s/point%u_%s.bin%s",
		 dir, point + 1, type, suffix);
}

static UINT32 nuc_prepare_table_transaction(
	NUC_TABLE_TRANSACTION_FILE *files, const char *dir, int include_raw)
{
	static const char *sensor_types[] = {"K", "B"};
	static const char *blackbody_types[] = {"K", "B", "RAW"};
	const char **types = include_raw ? blackbody_types : sensor_types;
	UINT32 type_count = include_raw ? 3U : 2U;
	UINT32 point;
	UINT32 type;

	memset(files, 0,
	       NUC_BLACKBODY_TABLE_FILE_COUNT *
	       sizeof(NUC_TABLE_TRANSACTION_FILE));
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		for (type = 0; type < type_count; type++) {
			UINT32 index = point * type_count + type;

			nuc_format_table_path(
				files[index].final_path,
				sizeof(files[index].final_path), dir, point,
				types[type], "");
			nuc_format_table_path(
				files[index].stage_path,
				sizeof(files[index].stage_path), dir, point,
				types[type], NUC_TABLE_STAGE_SUFFIX);
			nuc_format_table_path(
				files[index].backup_path,
				sizeof(files[index].backup_path), dir, point,
				types[type], NUC_TABLE_BACKUP_SUFFIX);
		}
	}

	return include_raw ? NUC_BLACKBODY_TABLE_FILE_COUNT :
		NUC_SENSOR_TABLE_FILE_COUNT;
}

static int nuc_cleanup_staged_tables(const char *dir, int include_raw)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_BLACKBODY_TABLE_FILE_COUNT];
	UINT32 file_count;
	UINT32 i;
	int ret = 0;
	int changed = 0;

	file_count = nuc_prepare_table_transaction(files, dir, include_raw);
	for (i = 0; i < file_count; i++) {
		if (remove(files[i].stage_path) == 0) {
			changed = 1;
		} else if (errno != ENOENT) {
			printf("remove staged NUC table fail: %s\n",
			       files[i].stage_path);
			ret = -1;
		}
	}
	if (changed) {
		nuc_sync_table_dir(dir);
	}

	return ret;
}

static int nuc_cleanup_staged_sensor_temp_tables(void)
{
	return nuc_cleanup_staged_tables(NUC_SENSOR_TEMP_DIR, 0);
}

static int nuc_cleanup_staged_blackbody_temp_tables(void)
{
	return nuc_cleanup_staged_tables(NUC_BLACKBODY_TEMP_DIR, 1);
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

static int nuc_recover_tables(const char *dir, const char *name,
			      int include_raw)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_BLACKBODY_TABLE_FILE_COUNT];
	int stage_exists[NUC_BLACKBODY_TABLE_FILE_COUNT] = {0};
	int backup_exists[NUC_BLACKBODY_TABLE_FILE_COUNT] = {0};
	UINT32 file_count;
	UINT32 i;
	int stage_found = 0;
	int backup_found = 0;
	int interrupted = 0;
	int changed = 0;
	int ret = 0;

	if (access(dir, F_OK) != 0) {
		if (errno == ENOENT) {
			return 0;
		}
		printf("check %s table directory fail: %s\n",
		       name, strerror(errno));
		return -1;
	}

	file_count = nuc_prepare_table_transaction(files, dir, include_raw);
	for (i = 0; i < file_count; i++) {
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
		printf("Interrupted %s table update found, restoring previous tables\n",
		       name);
		for (i = 0; i < file_count; i++) {
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
				nuc_sync_table_dir(dir);
			}
			return -1;
		}
	} else if (stage_found) {
		/* Calibration stopped before any final table was backed up. */
		printf("Incomplete staged NUC tables found, discarding stages\n");
	}

	if (nuc_cleanup_staged_tables(dir, include_raw) != 0) {
		ret = -1;
	}
	for (i = 0; i < file_count; i++) {
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
		nuc_sync_table_dir(dir);
	}
	if (interrupted && ret == 0) {
		printf("Previous %s tables restored\n", name);
	}

	return ret;
}

static int nuc_recover_sensor_temp_tables(void)
{
	return nuc_recover_tables(NUC_SENSOR_TEMP_DIR,
				  "NUC Sensor Temperature", 0);
}

static int nuc_recover_blackbody_temp_tables(void)
{
	return nuc_recover_tables(NUC_BLACKBODY_TEMP_DIR,
				  "NUC blackbody temperature", 1);
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

static int nuc_validate_all_blackbody_temp_tables(void)
{
	char path[128];
	static const char *types[] = {"RAW", "K", "B"};
	UINT32 point;
	UINT32 type;

	if (g_nuc.table_size == 0) {
		printf("NUC table size is not ready\n");
		return -1;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		for (type = 0; type < 3U; type++) {
			nuc_format_table_path(path, sizeof(path),
				NUC_BLACKBODY_TEMP_DIR, point, types[type], "");
			if (nuc_validate_table_file(path) != 0) {
				printf("Blackbody temperature point %u %s table is incomplete\n",
				       point + 1, types[type]);
				return -1;
			}
		}
	}

	printf("All %u blackbody temperature RAW/K/B sets are valid\n",
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

static int nuc_make_blackbody_temp_dir(void)
{
	if (nuc_make_output_dir() != 0) {
		return -1;
	}
	if (mkdir(NUC_BLACKBODY_TEMP_DIR, 0755) != 0 &&
	    access(NUC_BLACKBODY_TEMP_DIR, F_OK) != 0) {
		printf("create dir (%s) fail\n", NUC_BLACKBODY_TEMP_DIR);
		return -1;
	}

	return 0;
}

static int nuc_wait_sensor_temp_point(UINT32 point)
{
	char buf[16];

	printf("\nSensor temperature calibration point %u (capture once).\n",
	       point + 1);
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

static int nuc_build_blackbody_point_table(const UINT16 *average_raw,
					   UINT16 *k_tbl,
					   UINT16 *b_tbl)
{
	UINT64 sum = 0;
	UINT32 mean;
	UINT32 active_pixel_count;
	UINT32 clipped_count = 0;
	UINT32 x;
	UINT32 y;

	/* b_tbl may reuse average_raw; k_tbl must remain a separate buffer. */
	if (average_raw == NULL || k_tbl == NULL || b_tbl == NULL ||
	    average_raw == k_tbl || k_tbl == b_tbl ||
	    nuc_validate_image_layout() != 0) {
		return -1;
	}

	active_pixel_count = g_nuc.active_width * g_nuc.active_height;
	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;

		for (x = 0; x < g_nuc.active_width; x++) {
			sum += average_raw[row + x];
		}
	}
	mean = (UINT32)(sum / active_pixel_count);

	for (y = 0; y < g_nuc.raw_height; y++) {
		for (x = 0; x < g_nuc.raw_width; x++) {
			UINT32 pixel = y * g_nuc.raw_width + x;
			int active = x >= g_nuc.active_x &&
				x < g_nuc.active_x + g_nuc.active_width &&
				y >= g_nuc.active_y &&
				y < g_nuc.active_y + g_nuc.active_height;

			k_tbl[pixel] = (UINT16)NUC_GAIN_ONE;
			if (active) {
				long long offset =
					(long long)average_raw[pixel] - mean;

				if (offset < -32768 || offset > 32767) {
					clipped_count++;
				}
				b_tbl[pixel] = nuc_store_b_i64(offset);
			} else {
				b_tbl[pixel] = 0;
			}
		}
	}

	printf("NUC blackbody one-point active %ux%u mean=%u, clipped B=%u\n",
	       g_nuc.active_width, g_nuc.active_height, mean, clipped_count);
	return 0;
}

static int nuc_stage_sensor_temp_table(UINT32 point)
{
	char k_path[128];
	char b_path[128];

	if (nuc_make_sensor_temp_dir() != 0) {
		return -1;
	}

	nuc_format_table_path(k_path, sizeof(k_path), NUC_SENSOR_TEMP_DIR,
			      point, "K", NUC_TABLE_STAGE_SUFFIX);
	nuc_format_table_path(b_path, sizeof(b_path), NUC_SENSOR_TEMP_DIR,
			      point, "B", NUC_TABLE_STAGE_SUFFIX);

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
	nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);

	printf("Sensor temperature point %u K/B table staged\n", point + 1);
	return 0;
}

static int nuc_stage_blackbody_temp_table(UINT32 point,
					  const UINT16 *k_tbl,
					  const UINT16 *b_tbl)
{
	char k_path[128];
	char b_path[128];

	if (point >= NUC_FIVE_POINT_COUNT || k_tbl == NULL || b_tbl == NULL) {
		return -1;
	}
	if (nuc_make_blackbody_temp_dir() != 0) {
		return -1;
	}

	nuc_format_table_path(k_path, sizeof(k_path), NUC_BLACKBODY_TEMP_DIR,
			      point, "K", NUC_TABLE_STAGE_SUFFIX);
	nuc_format_table_path(b_path, sizeof(b_path), NUC_BLACKBODY_TEMP_DIR,
			      point, "B", NUC_TABLE_STAGE_SUFFIX);

	if (nuc_write_mem_to_file(k_path, k_tbl, g_nuc.table_size) != 0) {
		return -1;
	}
	if (nuc_write_mem_to_file(b_path, b_tbl, g_nuc.table_size) != 0) {
		return -1;
	}
	nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);

	printf("Blackbody temperature point %u K/B table staged\n", point + 1);
	return 0;
}

static int nuc_stage_blackbody_temp_raw(UINT32 point, const UINT16 *raw)
{
	char raw_path[128];

	if (point >= NUC_FIVE_POINT_COUNT || raw == NULL) {
		return -1;
	}
	if (nuc_make_blackbody_temp_dir() != 0) {
		return -1;
	}

	nuc_format_table_path(raw_path, sizeof(raw_path),
			      NUC_BLACKBODY_TEMP_DIR, point, "RAW",
			      NUC_TABLE_STAGE_SUFFIX);
	if (nuc_write_mem_to_file(raw_path, raw, g_nuc.table_size) != 0) {
		return -1;
	}
	nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);

	printf("Blackbody temperature point %u reference RAW staged\n",
	       point + 1);
	return 0;
}

static int nuc_commit_staged_sensor_temp_tables(const float *temperatures)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_BLACKBODY_TABLE_FILE_COUNT];
	UINT32 file_count;
	UINT32 i;
	UINT32 point;
	int ret = -1;

	file_count = nuc_prepare_table_transaction(
		files, NUC_SENSOR_TEMP_DIR, 0);
	for (i = 0; i < file_count; i++) {
		if (nuc_validate_table_file(files[i].stage_path) != 0) {
			printf("NUC staged table validation failed\n");
			goto rollback;
		}
	}
	nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);

	for (i = 0; i < file_count; i++) {
		if (access(files[i].final_path, F_OK) == 0) {
			if (rename(files[i].final_path,
				   files[i].backup_path) != 0) {
				printf("backup NUC table fail: %s\n",
				       files[i].final_path);
				goto rollback;
			}
			files[i].had_original = 1;
			nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);
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
		nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);
	}

	if (app_config_update_sensor_temp_points(
		    temperatures, NUC_FIVE_POINT_COUNT) != 0) {
		printf("save Sensor Temperature points to tiffany_thermal.cfg fail\n");
		goto rollback;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		g_nuc.sensor_temp_points[point] = temperatures[point];
	}
	for (i = 0; i < file_count; i++) {
		if (files[i].had_original &&
		    remove(files[i].backup_path) != 0 && errno != ENOENT) {
			printf("remove old NUC table backup fail: %s\n",
			       files[i].backup_path);
		}
	}
	nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);

	printf("All NUC Sensor Temperature K/B tables committed\n");
	return 0;

rollback:
	for (i = file_count; i > 0; i--) {
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
		nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);
	}
	nuc_cleanup_staged_sensor_temp_tables();
	nuc_sync_table_dir(NUC_SENSOR_TEMP_DIR);
	return ret;
}

static int nuc_commit_staged_blackbody_temp_tables(void)
{
	NUC_TABLE_TRANSACTION_FILE files[NUC_BLACKBODY_TABLE_FILE_COUNT];
	UINT32 file_count;
	UINT32 i;
	int ret = -1;

	file_count = nuc_prepare_table_transaction(
		files, NUC_BLACKBODY_TEMP_DIR, 1);
	for (i = 0; i < file_count; i++) {
		if (nuc_validate_table_file(files[i].stage_path) != 0) {
			printf("Blackbody NUC staged table validation failed\n");
			goto rollback;
		}
	}
	nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);

	for (i = 0; i < file_count; i++) {
		if (access(files[i].final_path, F_OK) == 0) {
			if (rename(files[i].final_path,
				   files[i].backup_path) != 0) {
				printf("backup blackbody NUC table fail: %s\n",
				       files[i].final_path);
				goto rollback;
			}
			files[i].had_original = 1;
			nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);
		} else if (errno != ENOENT) {
			printf("check blackbody NUC table fail: %s\n",
			       files[i].final_path);
			goto rollback;
		}

		if (rename(files[i].stage_path, files[i].final_path) != 0) {
			printf("install staged blackbody NUC table fail: %s\n",
			       files[i].final_path);
			goto rollback;
		}
		files[i].installed = 1;
		nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);
	}

	for (i = 0; i < file_count; i++) {
		if (files[i].had_original &&
		    remove(files[i].backup_path) != 0 && errno != ENOENT) {
			printf("remove old blackbody NUC table backup fail: %s\n",
			       files[i].backup_path);
		}
	}
	nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);

	printf("All blackbody temperature NUC RAW/K/B tables committed\n");
	return 0;

rollback:
	for (i = file_count; i > 0; i--) {
		NUC_TABLE_TRANSACTION_FILE *file = &files[i - 1U];

		if (file->installed &&
		    remove(file->final_path) != 0 && errno != ENOENT) {
			printf("remove new blackbody NUC table during rollback fail: %s\n",
			       file->final_path);
		}
		if (file->had_original &&
		    rename(file->backup_path, file->final_path) != 0) {
			printf("restore old blackbody NUC table fail: %s\n",
			       file->final_path);
		}
		nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);
	}
	nuc_cleanup_staged_blackbody_temp_tables();
	nuc_sync_table_dir(NUC_BLACKBODY_TEMP_DIR);
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

static UINT64 nuc_get_time_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (UINT64)ts.tv_sec * 1000000U +
		(UINT64)ts.tv_nsec / 1000U;
}

static UINT16 nuc_midpoint_boundary(UINT16 low, UINT16 high)
{
	UINT16 swap;

	/* Keep the performance-test fallback free from unsigned underflow. */
	if (high < low) {
		swap = low;
		low = high;
		high = swap;
	}
	/* Integer RAW below the midpoint uses the lower point; equality is higher. */
	return (UINT16)(low + ((UINT32)high - low + 1U) / 2U);
}

static int nuc_pixel_is_active(UINT32 pixel)
{
	UINT32 x = pixel % g_nuc.raw_width;
	UINT32 y = pixel / g_nuc.raw_width;

	return x >= g_nuc.active_x &&
		x < g_nuc.active_x + g_nuc.active_width &&
		y >= g_nuc.active_y &&
		y < g_nuc.active_y + g_nuc.active_height;
}

static UINT32 nuc_raw_active_mean(const UINT16 *raw)
{
	UINT64 sum = 0;
	UINT32 x;
	UINT32 y;

	for (y = 0; y < g_nuc.active_height; y++) {
		UINT32 row = (g_nuc.active_y + y) * g_nuc.raw_width +
			g_nuc.active_x;

		for (x = 0; x < g_nuc.active_width; x++) {
			sum += raw[row + x];
		}
	}

	return (UINT32)(sum /
		(g_nuc.active_width * g_nuc.active_height));
}

static void nuc_reset_dynamic_performance(void)
{
	g_nuc.dynamic_report_frames = 0;
	g_nuc.dynamic_map_us = 0;
	g_nuc.dynamic_select_us = 0;
	g_nuc.dynamic_flush_us = 0;
	g_nuc.dynamic_gfx_us = 0;
	g_nuc.dynamic_max_total_us = 0;
	memset(g_nuc.dynamic_point_pixels, 0,
	       sizeof(g_nuc.dynamic_point_pixels));
	g_nuc.dynamic_last_frame_start_us = 0;
	g_nuc.dynamic_interval_us = 0;
	g_nuc.dynamic_interval_count = 0;
	g_nuc.dynamic_pending_prepare_us = 0;
	g_nuc.dynamic_perf_pending = 0;
}

static void nuc_release_blackbody_dynamic_sources(void)
{
	UINT32 point;

	free(g_nuc.blackbody_source_tables);
	free(g_nuc.blackbody_thresholds);
	g_nuc.blackbody_source_tables = NULL;
	g_nuc.blackbody_thresholds = NULL;
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		g_nuc.blackbody_k[point] = NULL;
		g_nuc.blackbody_b[point] = NULL;
		g_nuc.blackbody_reference_mean[point] = 0;
	}
	g_nuc.blackbody_invalid_pixel_count = 0;
	g_nuc.blackbody_dynamic_enabled = 0;
	nuc_reset_dynamic_performance();
}

static int nuc_load_blackbody_dynamic_sources(void)
{
	UINT16 *source_tables = NULL;
	UINT16 *thresholds = NULL;
	UINT16 *previous_raw = NULL;
	UINT16 *current_raw = NULL;
	unsigned char *invalid_pixel = NULL;
	UINT16 *k_tables[NUC_FIVE_POINT_COUNT];
	UINT16 *b_tables[NUC_FIVE_POINT_COUNT];
	UINT32 reference_mean[NUC_FIVE_POINT_COUNT];
	UINT32 pixel_count;
	UINT32 invalid_count = 0;
	UINT32 point;
	UINT32 boundary;
	UINT32 pixel;
	size_t source_bytes;
	size_t threshold_bytes;
	char path[128];
	int reference_order_valid = 1;
	int ret = -1;

	if (nuc_validate_image_layout() != 0) {
		return -1;
	}
	if (nuc_recover_blackbody_temp_tables() != 0 ||
	    nuc_validate_all_blackbody_temp_tables() != 0) {
		return -1;
	}

	pixel_count = g_nuc.table_size / sizeof(UINT16);
	source_bytes = (size_t)g_nuc.table_size *
		NUC_FIVE_POINT_COUNT * 2U;
	threshold_bytes = (size_t)g_nuc.table_size *
		NUC_DYNAMIC_BOUNDARY_COUNT;
	source_tables = (UINT16 *)malloc(source_bytes);
	thresholds = (UINT16 *)malloc(threshold_bytes);
	previous_raw = (UINT16 *)malloc(g_nuc.table_size);
	current_raw = (UINT16 *)malloc(g_nuc.table_size);
	invalid_pixel = (unsigned char *)calloc(pixel_count,
						 sizeof(*invalid_pixel));
	if (source_tables == NULL || thresholds == NULL ||
	    previous_raw == NULL || current_raw == NULL ||
	    invalid_pixel == NULL) {
		printf("allocate blackbody dynamic NUC source tables fail\n");
		goto exit;
	}

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		k_tables[point] = source_tables +
			(size_t)(point * 2U) * pixel_count;
		b_tables[point] = source_tables +
			(size_t)(point * 2U + 1U) * pixel_count;

		nuc_format_table_path(path, sizeof(path),
				      NUC_BLACKBODY_TEMP_DIR, point, "K", "");
		if (nuc_read_file_to_mem(path, k_tables[point],
					 g_nuc.table_size) != 0) {
			goto exit;
		}
		nuc_format_table_path(path, sizeof(path),
				      NUC_BLACKBODY_TEMP_DIR, point, "B", "");
		if (nuc_read_file_to_mem(path, b_tables[point],
					 g_nuc.table_size) != 0) {
			goto exit;
		}
	}

	nuc_format_table_path(path, sizeof(path), NUC_BLACKBODY_TEMP_DIR,
			      0, "RAW", "");
	if (nuc_read_file_to_mem(path, previous_raw, g_nuc.table_size) != 0) {
		goto exit;
	}
	reference_mean[0] = nuc_raw_active_mean(previous_raw);

	for (point = 1; point < NUC_FIVE_POINT_COUNT; point++) {
		UINT16 *swap_raw;
		UINT16 *point_threshold = thresholds +
			(size_t)(point - 1U) * pixel_count;

		nuc_format_table_path(path, sizeof(path),
				      NUC_BLACKBODY_TEMP_DIR, point, "RAW", "");
		if (nuc_read_file_to_mem(path, current_raw,
					 g_nuc.table_size) != 0) {
			goto exit;
		}
		reference_mean[point] = nuc_raw_active_mean(current_raw);

		for (pixel = 0; pixel < pixel_count; pixel++) {
			if (current_raw[pixel] > previous_raw[pixel]) {
				point_threshold[pixel] = nuc_midpoint_boundary(
					previous_raw[pixel], current_raw[pixel]);
			} else if (nuc_pixel_is_active(pixel)) {
				invalid_pixel[pixel] = 1;
			}
		}

		swap_raw = previous_raw;
		previous_raw = current_raw;
		current_raw = swap_raw;
	}

	for (point = 1; point < NUC_FIVE_POINT_COUNT; point++) {
		if (reference_mean[point] <= reference_mean[point - 1U]) {
			printf("Blackbody reference RAW means are not low-to-high: point %u=%u, point %u=%u\n",
			       point, reference_mean[point - 1U],
			       point + 1U, reference_mean[point]);
			reference_order_valid = 0;
#if !NUC_DYNAMIC_PERF_ALLOW_NON_MONOTONIC
			goto exit;
#endif
		}
	}
	if (!reference_order_valid) {
		printf("WARNING: non-monotonic Blackbody RAW bypassed for performance test only; correction quality and point distribution are invalid\n");
	}

	for (pixel = 0; pixel < pixel_count; pixel++) {
		if (nuc_pixel_is_active(pixel) && !invalid_pixel[pixel]) {
			continue;
		}
		if (invalid_pixel[pixel]) {
			invalid_count++;
		}
		for (boundary = 0; boundary < NUC_DYNAMIC_BOUNDARY_COUNT;
		     boundary++) {
			thresholds[(size_t)boundary * pixel_count + pixel] =
				nuc_midpoint_boundary(
					(UINT16)reference_mean[boundary],
					(UINT16)reference_mean[boundary + 1U]);
		}
	}

	nuc_release_blackbody_dynamic_sources();
	g_nuc.blackbody_source_tables = source_tables;
	g_nuc.blackbody_thresholds = thresholds;
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		g_nuc.blackbody_k[point] = k_tables[point];
		g_nuc.blackbody_b[point] = b_tables[point];
		g_nuc.blackbody_reference_mean[point] = reference_mean[point];
	}
	g_nuc.blackbody_invalid_pixel_count = invalid_count;
	source_tables = NULL;
	thresholds = NULL;

	printf("Blackbody dynamic NUC loaded: K/B=%zu bytes, thresholds=%zu bytes, invalid pixels=%u\n",
	       source_bytes, threshold_bytes, invalid_count);
	printf("Blackbody reference RAW means: %u, %u, %u, %u, %u\n",
	       reference_mean[0], reference_mean[1], reference_mean[2],
	       reference_mean[3], reference_mean[4]);
	ret = 0;

exit:
	free(source_tables);
	free(thresholds);
	free(previous_raw);
	free(current_raw);
	free(invalid_pixel);
	return ret;
}

int nuc_prepare_blackbody_dynamic_table(HD_VIDEO_FRAME *in_frame)
{
	UINTPTR block_pa;
	UINTPTR map_va;
	UINTPTR raw_va;
	UINT32 stride;
	UINT32 plane_height;
	UINT32 map_size;
	UINT32 pixel_count;
	UINT32 x;
	UINT32 y;
	UINT64 point_pixels[NUC_FIVE_POINT_COUNT] = {0};
	UINT64 start_us;
	UINT64 map_ready_us;
	UINT64 select_done_us;
	UINT64 unmap_done_us;
	UINT64 done_us;
	UINT64 map_size_64;
	UINT16 *dynamic_k;
	UINT16 *dynamic_b;
	HD_RESULT unmap_ret;

	if (!g_nuc.blackbody_dynamic_enabled) {
		return 0;
	}
	if (in_frame == NULL || in_frame->blk == 0 ||
	    g_nuc.blackbody_source_tables == NULL ||
	    g_nuc.blackbody_thresholds == NULL ||
	    g_nuc.k_va == 0 || g_nuc.b_va == 0) {
		printf("Blackbody dynamic NUC runtime is not ready\n");
		return -1;
	}

	stride = in_frame->loff[0];
	if (stride < in_frame->dim.w * sizeof(UINT16)) {
		stride *= sizeof(UINT16);
	}
	plane_height = in_frame->ph[0] != 0 ?
		in_frame->ph[0] : in_frame->dim.h;
	if (stride < g_nuc.raw_width * sizeof(UINT16) ||
	    plane_height < g_nuc.raw_height) {
		printf("Blackbody dynamic NUC invalid frame layout: stride=%u, height=%u\n",
		       stride, plane_height);
		return -1;
	}

	block_pa = hd_common_mem_blk2pa(in_frame->blk);
	if (block_pa == 0 || in_frame->phy_addr[0] < block_pa) {
		printf("Blackbody dynamic NUC blk2pa fail\n");
		return -1;
	}
	map_size_64 = (UINT64)(in_frame->phy_addr[0] - block_pa) +
		(UINT64)stride * plane_height;
	if (map_size_64 > UINT_MAX) {
		printf("Blackbody dynamic NUC map size is too large\n");
		return -1;
	}
	map_size = (UINT32)map_size_64;

	start_us = nuc_get_time_us();
	map_va = (UINTPTR)hd_common_mem_mmap(
		HD_COMMON_MEM_MEM_TYPE_CACHE, block_pa, map_size);
	if (map_va == 0) {
		printf("Blackbody dynamic NUC mmap fail\n");
		return -1;
	}
	hd_common_mem_flush_cache((void *)map_va, map_size);
	raw_va = map_va + (in_frame->phy_addr[0] - block_pa);
	map_ready_us = nuc_get_time_us();

	pixel_count = g_nuc.raw_width * g_nuc.raw_height;
	dynamic_k = (UINT16 *)g_nuc.k_va;
	dynamic_b = (UINT16 *)g_nuc.b_va;
	for (y = 0; y < g_nuc.raw_height; y++) {
		const UINT16 *raw_row =
			(const UINT16 *)(raw_va + (UINTPTR)y * stride);
		UINT32 row = y * g_nuc.raw_width;

		for (x = 0; x < g_nuc.raw_width; x++) {
			UINT32 pixel = row + x;
			UINT16 raw = raw_row[x];
			UINT32 point = 0;

			while (point < NUC_DYNAMIC_BOUNDARY_COUNT &&
			       raw >= g_nuc.blackbody_thresholds[
				       (size_t)point * pixel_count + pixel]) {
				point++;
			}
			dynamic_k[pixel] = g_nuc.blackbody_k[point][pixel];
			dynamic_b[pixel] = g_nuc.blackbody_b[point][pixel];
			point_pixels[point]++;
		}
	}
	select_done_us = nuc_get_time_us();

	unmap_ret = hd_common_mem_munmap((void *)map_va, map_size);
	if (unmap_ret != HD_OK) {
		printf("Blackbody dynamic NUC munmap fail=%d\n", unmap_ret);
	}
	unmap_done_us = nuc_get_time_us();
	hd_common_mem_flush_cache((void *)g_nuc.k_va, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.b_va, g_nuc.table_size);
	done_us = nuc_get_time_us();

	if (g_nuc.dynamic_last_frame_start_us != 0 &&
	    start_us >= g_nuc.dynamic_last_frame_start_us) {
		g_nuc.dynamic_interval_us +=
			start_us - g_nuc.dynamic_last_frame_start_us;
		g_nuc.dynamic_interval_count++;
	}
	g_nuc.dynamic_last_frame_start_us = start_us;
	g_nuc.dynamic_map_us += (map_ready_us - start_us) +
		(unmap_done_us - select_done_us);
	g_nuc.dynamic_select_us += select_done_us - map_ready_us;
	g_nuc.dynamic_flush_us += done_us - unmap_done_us;
	for (x = 0; x < NUC_FIVE_POINT_COUNT; x++) {
		g_nuc.dynamic_point_pixels[x] += point_pixels[x];
	}
	g_nuc.dynamic_pending_prepare_us = done_us - start_us;
	g_nuc.dynamic_perf_pending = 1;
	return 0;
}

void nuc_bind_runtime(UINTPTR k_va, UINTPTR b_va, UINT32 table_size,
		      const NUC_IMAGE_LAYOUT *layout)
{
	nuc_release_blackbody_dynamic_sources();
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

int nuc_set_blackbody_dynamic_enabled(int enable)
{
	NUC_CALIBRATION_STATE_CB calibration_state = g_nuc.calibration_state;
	int requested = enable ? 1 : 0;
	int ret = 0;

	if (requested == g_nuc.blackbody_dynamic_enabled) {
		printf("Blackbody dynamic NUC is already %s\n",
		       requested ? "on" : "off");
		return 0;
	}

	if (requested) {
		if (nuc_load_blackbody_dynamic_sources() != 0) {
			printf("Enable blackbody dynamic NUC fail\n");
			return -1;
		}
		if (calibration_state != NULL) {
			calibration_state("NUC dynamic mode", 1);
		}
		g_nuc.blackbody_dynamic_enabled = 1;
		g_nuc.table_loaded = 1;
		g_nuc.enabled = 1;
		nuc_reset_dynamic_performance();
		if (calibration_state != NULL) {
			calibration_state("NUC dynamic mode", 0);
		}
		printf("Blackbody dynamic NUC on; NUC master is on\n");
		return 0;
	}

	if (calibration_state != NULL) {
		calibration_state("NUC dynamic mode", 1);
	}
	g_nuc.blackbody_dynamic_enabled = 0;
	nuc_release_blackbody_dynamic_sources();
	ret = nuc_reload_table();
	if (calibration_state != NULL) {
		calibration_state("NUC dynamic mode", 0);
	}
	if (ret == 0) {
		printf("Blackbody dynamic NUC off; Sensor Temperature table restored\n");
	} else {
		printf("Blackbody dynamic NUC off; restore Sensor Temperature table fail\n");
	}
	return ret;
}

int nuc_is_blackbody_dynamic_enabled(void)
{
	return g_nuc.blackbody_dynamic_enabled;
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

static int nuc_load_sensor_temp_table(UINT32 point, int verbose)
{
	char k_path[128];
	char b_path[128];
	UINT16 *k_table = 0;
	UINT16 *b_table = 0;
	int ret = -1;

	if (point >= NUC_FIVE_POINT_COUNT) {
		if (verbose) {
			printf("invalid NUC Sensor Temperature table point %u\n",
			       point + 1);
		}
		return -1;
	}
	if (g_nuc.table_size == 0) {
		if (verbose) {
			printf("NUC table size is not ready\n");
		}
		return -1;
	}

	if (g_nuc.k_va == 0) {
		if (verbose) {
			printf("NUC K table buffer is not ready\n");
		}
		return -1;
	}

	if (g_nuc.b_va == 0) {
		if (verbose) {
			printf("NUC B table buffer is not ready\n");
		}
		return -1;
	}

	snprintf(k_path, sizeof(k_path), "%s/point%u_K.bin",
		 NUC_SENSOR_TEMP_DIR, point + 1);
	snprintf(b_path, sizeof(b_path), "%s/point%u_B.bin",
		 NUC_SENSOR_TEMP_DIR, point + 1);
	if (nuc_validate_table_file_internal(k_path, verbose) != 0 ||
	    nuc_validate_table_file_internal(b_path, verbose) != 0) {
		return -1;
	}

	k_table = (UINT16 *)malloc(g_nuc.table_size);
	b_table = (UINT16 *)malloc(g_nuc.table_size);
	if (k_table == 0 || b_table == 0) {
		if (verbose) {
			printf("allocate NUC Sensor Temperature table fail\n");
		}
		goto exit;
	}

	if (nuc_read_file_to_mem_internal(
		    k_path, k_table, g_nuc.table_size, verbose) != 0 ||
	    nuc_read_file_to_mem_internal(
		    b_path, b_table, g_nuc.table_size, verbose) != 0) {
		goto exit;
	}

	memcpy((void *)g_nuc.k_va, k_table, g_nuc.table_size);
	memcpy((void *)g_nuc.b_va, b_table, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.k_va, g_nuc.table_size);
	hd_common_mem_flush_cache((void *)g_nuc.b_va, g_nuc.table_size);

	g_nuc.table_loaded = 1;
	g_nuc.sensor_temp_table_point = (int)point;
	if (verbose) {
		printf("NUC Sensor Temperature point %u K/B table loaded\n",
		       point + 1);
	}
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

	if (g_nuc.blackbody_dynamic_enabled) {
		printf("Sensor Temperature reload skipped while blackbody dynamic NUC is on\n");
		return -1;
	}

	if (nuc_validate_all_sensor_temp_tables() != 0) {
		goto fail;
	}

	if (!nuc_sensor_temp_points_are_ready()) {
		printf("NUC Sensor Temperature points are not ready, load point 1\n");
		ret = nuc_load_sensor_temp_table(0, 1);
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

static int nuc_update_sensor_temp_table_internal(int verbose)
{
	float temp_c;
	UINT32 point;

	if (g_nuc.blackbody_dynamic_enabled) {
		return 0;
	}

	if (!nuc_sensor_temp_points_are_ready()) {
		if (verbose) {
			printf("NUC Sensor Temperature points are not ready\n");
		}
		return -1;
	}

	if (sensor_temp_read(&temp_c) != 0) {
		if (verbose) {
			printf("read runtime sensor temperature fail\n");
		}
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

	if (nuc_load_sensor_temp_table(point, verbose) != 0) {
		g_nuc.table_loaded = 0;
		nuc_set_enabled(0);
		return -1;
	}

	if (verbose) {
		printf("NUC Sensor Temperature %.4f C, use point %u (%.4f C)\n",
		       temp_c, point + 1, g_nuc.sensor_temp_points[point]);
	}
	return 0;
}

int nuc_update_sensor_temp_table(void)
{
	return nuc_update_sensor_temp_table_internal(1);
}

int nuc_update_sensor_temp_table_periodic(void)
{
	return nuc_update_sensor_temp_table_internal(0);
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
	if (g_nuc.blackbody_dynamic_enabled) {
		printf("Disable blackbody dynamic NUC before Sensor Temperature calibration\n");
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

	/* Pair the new NUC tables with the latest saved BPR table. */
	if (app_config_update_int("nuc_correction", 0) != 0 ||
	    nuc_is_enabled()) {
		printf("NUC calibration aborted: cannot turn NUC off\n");
		goto exit;
	}
	if (bpr_load_table() != 0 ||
	    app_config_update_int("bpr_correction", 1) != 0 ||
	    !bpr_is_enabled()) {
		printf("NUC calibration aborted: latest BPR table must be loaded and enabled\n");
		goto exit;
	}
	printf("NUC calibration: latest BPR table loaded, BPR on, NUC off (CFG saved)\n");

	active_pixel_count = g_nuc.active_width * g_nuc.active_height;
	low_raw = (UINT16 *)malloc(g_nuc.table_size);
	high_raw = (UINT16 *)malloc(g_nuc.table_size);
	frame_raw = (UINT16 *)malloc(g_nuc.table_size);
	raw_sum = (UINT32 *)malloc(active_pixel_count * sizeof(*raw_sum));
	if (low_raw == 0 || high_raw == 0 || frame_raw == 0 || raw_sum == 0) {
		printf("5 point Sensor Temperature NUC calibration malloc fail\n");
		goto exit;
	}

	printf("Perform Low/High calibration once; reuse identical K/B for points 1-5.\n");
	if (nuc_wait_sensor_temp_point(0) != 0) {
		goto exit;
	}
	if (sensor_temp_read(&temperatures[0]) != 0) {
		printf("read sensor temperature point 1 fail\n");
		goto exit;
	}
	printf("Sensor temperature = %.4f C (same value saved for points 1-5)\n",
	       temperatures[0]);

	if (nuc_capture_two_point(low_raw, high_raw, frame_raw, raw_sum) != 0) {
		printf("capture Low/High temperature RAW fail\n");
		goto exit;
	}
	if (nuc_build_table(low_raw, high_raw,
			    (UINT16 *)g_nuc.k_va,
			    (UINT16 *)g_nuc.b_va) != 0) {
		printf("NUC calibration rejected; abort calibration\n");
		goto exit;
	}

	/* Preserve the five-file-pair format without repeating acquisition.
	 * Equal sensor temperatures intentionally use the existing point-1 fallback.
	 */
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		temperatures[point] = temperatures[0];
		if (nuc_stage_sensor_temp_table(point) != 0) {
			goto exit;
		}
	}

	if (nuc_commit_staged_sensor_temp_tables(temperatures) != 0) {
		printf("commit Sensor Temperature NUC tables fail\n");
		goto exit;
	}

	printf("Sensor Temperature NUC calibration done: identical K/B saved to points 1-5\n");
	ret = 0;

exit:
	if (ret != 0) {
		nuc_cleanup_staged_sensor_temp_tables();
	}
	free(low_raw);
	free(high_raw);
	free(frame_raw);
	free(raw_sum);

	if (nuc_reload_table() != 0) {
		ret = -1;
	}
	if (ret == 0) {
		if (app_config_update_int("nuc_correction", 1) != 0 ||
		    !nuc_is_enabled()) {
			printf("NUC tables saved but enabling NUC failed\n");
			ret = -1;
		} else {
			printf("NUC calibration complete: BPR on, NUC on (CFG saved)\n");
		}
	} else {
		printf("NUC calibration failed; NUC was not automatically enabled\n");
	}
	if (calibration_state != 0) {
		calibration_state("NUC", 0);
	}

	return ret;
}

int nuc_five_point_blackbody_temp_calibration(void)
{
	UINT16 *average_raw = NULL;
	UINT16 *frame_raw = NULL;
	UINT32 *raw_sum = NULL;
	NUC_CALIBRATION_STATE_CB calibration_state = NULL;
	UINT32 active_pixel_count;
	UINT32 point;
	int ret = -1;

	if (g_nuc.table_size == 0 || g_nuc.k_va == 0 || g_nuc.b_va == 0) {
		printf("NUC runtime buffer is not ready\n");
		return -1;
	}
	if (g_nuc.blackbody_dynamic_enabled) {
		printf("Disable blackbody dynamic NUC before recalibration\n");
		return -1;
	}
	if (g_nuc.capture_raw == NULL) {
		printf("NUC capture callback is not ready\n");
		return -1;
	}
	if (nuc_validate_image_layout() != 0) {
		return -1;
	}
	if (nuc_recover_blackbody_temp_tables() != 0) {
		printf("recover blackbody temperature NUC tables fail\n");
		return -1;
	}

	calibration_state = g_nuc.calibration_state;
	if (calibration_state != NULL) {
		calibration_state("NUC blackbody", 1);
	}

	active_pixel_count = g_nuc.active_width * g_nuc.active_height;
	average_raw = (UINT16 *)malloc(g_nuc.table_size);
	frame_raw = (UINT16 *)malloc(g_nuc.table_size);
	raw_sum = (UINT32 *)malloc(active_pixel_count * sizeof(*raw_sum));
	if (average_raw == NULL || frame_raw == NULL || raw_sum == NULL) {
		printf("5 point blackbody temperature NUC calibration malloc fail\n");
		goto exit;
	}
	printf("Capture the five blackbody points from low to high temperature.\n");

	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		char point_name[64];

		snprintf(point_name, sizeof(point_name),
			 "temperature point %u/%u", point + 1,
			 NUC_FIVE_POINT_COUNT);
		if (nuc_capture_average_raw(point_name, average_raw,
					    frame_raw, raw_sum) != 0) {
			printf("capture blackbody temperature point %u fail\n",
			       point + 1);
			goto exit;
		}
		if (nuc_stage_blackbody_temp_raw(point, average_raw) != 0) {
			printf("save blackbody temperature point %u reference RAW fail\n",
			       point + 1);
			goto exit;
		}

		/* Reuse frame_raw as K and average_raw as the in-place B table. */
		if (nuc_build_blackbody_point_table(average_raw, frame_raw,
						    average_raw) != 0) {
			printf("build blackbody temperature point %u table fail\n",
			       point + 1);
			goto exit;
		}
		if (nuc_stage_blackbody_temp_table(point, frame_raw,
						   average_raw) != 0) {
			printf("save blackbody temperature point %u table fail\n",
			       point + 1);
			goto exit;
		}
	}

	if (nuc_commit_staged_blackbody_temp_tables() != 0) {
		printf("commit blackbody temperature NUC tables fail\n");
		goto exit;
	}

	printf("5 point blackbody temperature NUC RAW/K/B saved to %s\n",
	       NUC_BLACKBODY_TEMP_DIR);
	ret = 0;

exit:
	if (ret != 0) {
		nuc_cleanup_staged_blackbody_temp_tables();
	}
	free(average_raw);
	free(frame_raw);
	free(raw_sum);

	if (calibration_state != NULL) {
		calibration_state("NUC blackbody", 0);
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

static int nuc_reload_table_from_menu(void)
{
	NUC_CALIBRATION_STATE_CB calibration_state = g_nuc.calibration_state;
	int ret;

	if (calibration_state != NULL) {
		calibration_state("NUC table reload", 1);
	}
	ret = nuc_reload_table();
	if (calibration_state != NULL) {
		calibration_state("NUC table reload", 0);
	}

	return ret;
}

int nuc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("NUC menu\n");
		printf(" 1. Sensor Temperature NUC calibration (capture once, copy to 5 points)\n");
		printf(" 2. 5 point Blackbody Temperature NUC calibration (save RAW/K/B)\n");
		printf(" 3. Toggle per-pixel Blackbody Dynamic NUC\n");
		printf(" 4. Reload Sensor Temperature K/B table\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: NUC=%s, table mode=%s, Sensor Temperature interval=%u sec\n",
		       g_nuc.enabled ? "on" : "off",
		       g_nuc.blackbody_dynamic_enabled ?
			"Blackbody Dynamic" : "Sensor Temperature",
		       g_nuc_sensor_temp_interval_sec);
		printf("Settings: CFG Setting -> NUC setting\n");

		option = nuc_read_option();
		switch (option) {
		case 1:
			ret = nuc_five_point_sensor_temp_calibration();
			break;

		case 2:
			ret = nuc_five_point_blackbody_temp_calibration();
			break;

		case 3:
			ret = nuc_set_blackbody_dynamic_enabled(
				!nuc_is_blackbody_dynamic_enabled());
			break;

		case 4:
			ret = nuc_reload_table_from_menu();
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

static void nuc_finish_dynamic_performance(UINT64 gfx_us)
{
	UINT64 total_us;
	UINT64 total_pixels = 0;
	UINT32 point;
	double frames;
	double fps = 0.0;

	if (!g_nuc.dynamic_perf_pending) {
		return;
	}

	total_us = g_nuc.dynamic_pending_prepare_us + gfx_us;
	g_nuc.dynamic_gfx_us += gfx_us;
	if (total_us > g_nuc.dynamic_max_total_us) {
		g_nuc.dynamic_max_total_us = total_us;
	}
	g_nuc.dynamic_report_frames++;
	g_nuc.dynamic_perf_pending = 0;
	g_nuc.dynamic_pending_prepare_us = 0;

	if (g_nuc.dynamic_report_frames < NUC_DYNAMIC_REPORT_FRAME_COUNT) {
		return;
	}

	frames = (double)g_nuc.dynamic_report_frames;
	if (g_nuc.dynamic_interval_us != 0) {
		fps = (double)g_nuc.dynamic_interval_count * 1000000.0 /
			(double)g_nuc.dynamic_interval_us;
	}
	for (point = 0; point < NUC_FIVE_POINT_COUNT; point++) {
		total_pixels += g_nuc.dynamic_point_pixels[point];
	}

	printf("Dynamic NUC perf (%" PRIu64 " frames): map/cache=%.3f ms, select=%.3f ms, flush=%.3f ms, CPU added=%.3f ms, GFX=%.3f ms, NUC total=%.3f ms, max=%.3f ms, pipeline=%.2f FPS\n",
	       (uint64_t)g_nuc.dynamic_report_frames,
	       (double)g_nuc.dynamic_map_us / frames / 1000.0,
	       (double)g_nuc.dynamic_select_us / frames / 1000.0,
	       (double)g_nuc.dynamic_flush_us / frames / 1000.0,
	       (double)(g_nuc.dynamic_map_us + g_nuc.dynamic_select_us +
			g_nuc.dynamic_flush_us) / frames / 1000.0,
	       (double)g_nuc.dynamic_gfx_us / frames / 1000.0,
	       (double)(g_nuc.dynamic_map_us + g_nuc.dynamic_select_us +
			g_nuc.dynamic_flush_us + g_nuc.dynamic_gfx_us) /
		frames / 1000.0,
	       (double)g_nuc.dynamic_max_total_us / 1000.0, fps);
	printf("Dynamic NUC point distribution: P1=%" PRIu64 " (%.2f%%), P2=%" PRIu64 " (%.2f%%), P3=%" PRIu64 " (%.2f%%), P4=%" PRIu64 " (%.2f%%), P5=%" PRIu64 " (%.2f%%)\n",
	       (uint64_t)g_nuc.dynamic_point_pixels[0],
	       total_pixels ? 100.0 * g_nuc.dynamic_point_pixels[0] /
		(double)total_pixels : 0.0,
	       (uint64_t)g_nuc.dynamic_point_pixels[1],
	       total_pixels ? 100.0 * g_nuc.dynamic_point_pixels[1] /
		(double)total_pixels : 0.0,
	       (uint64_t)g_nuc.dynamic_point_pixels[2],
	       total_pixels ? 100.0 * g_nuc.dynamic_point_pixels[2] /
		(double)total_pixels : 0.0,
	       (uint64_t)g_nuc.dynamic_point_pixels[3],
	       total_pixels ? 100.0 * g_nuc.dynamic_point_pixels[3] /
		(double)total_pixels : 0.0,
	       (uint64_t)g_nuc.dynamic_point_pixels[4],
	       total_pixels ? 100.0 * g_nuc.dynamic_point_pixels[4] /
		(double)total_pixels : 0.0);

	g_nuc.dynamic_report_frames = 0;
	g_nuc.dynamic_map_us = 0;
	g_nuc.dynamic_select_us = 0;
	g_nuc.dynamic_flush_us = 0;
	g_nuc.dynamic_gfx_us = 0;
	g_nuc.dynamic_max_total_us = 0;
	memset(g_nuc.dynamic_point_pixels, 0,
	       sizeof(g_nuc.dynamic_point_pixels));
	g_nuc.dynamic_interval_us = 0;
	g_nuc.dynamic_interval_count = 0;
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
	UINT64 gfx_start_us = 0;
	UINT64 gfx_done_us;
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

	if (g_nuc.dynamic_perf_pending) {
		gfx_start_us = nuc_get_time_us();
	}
	ret = vendor_gfx_grph_trigger(1, &param);
	if (g_nuc.dynamic_perf_pending) {
		gfx_done_us = nuc_get_time_us();
		nuc_finish_dynamic_performance(gfx_done_us - gfx_start_us);
	}
	if (ret != HD_OK) {
		printf("vendor_gfx_grph_trigger fail=%d\n", ret);
	}

	out_frame->phy_addr[0] = out_pa;
	out_frame->blk = out_blk;

	return ret;
}
