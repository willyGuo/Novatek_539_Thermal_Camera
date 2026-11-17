#include "ffc.h"
#include "ffc_shading_math.h"
#include "../../Config/hardware_config.h"
#include "../NUC/nuc_gain_format.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__LINUX)
#include <pthread.h>
#else
#include <FreeRTOS_POSIX.h>
#include <FreeRTOS_POSIX/pthread.h>
#endif

#include "vendor_gfx.h"

#define SHUTTER_DEFAULT_INTERVAL_SEC 60U
#define MENU_OPTION_REFRESH (-2)

typedef struct {
	UINTPTR b_va;
	UINT32 table_size;
	UINT32 shutter_interval_sec;
	FFC_TEST_CB run_test;
	int enabled;
	int startup_enabled;
	int table_ready;
} FFC_CONTEXT;

static FFC_CONTEXT g_ffc = {
	.shutter_interval_sec = SHUTTER_DEFAULT_INTERVAL_SEC,
	.enabled = 1,
	.startup_enabled = 1,
};

#ifndef FFC_SHADING_DIR
#define FFC_SHADING_DIR "/mnt/sd/thermal_op"
#endif
#define FFC_SHADING_PATH FFC_SHADING_DIR "/ffc_shading.bin"
#define FFC_SHADING_NEW FFC_SHADING_PATH ".new"
#define FFC_SHADING_PIXELS ((size_t)THERMAL_SENSOR_WIDTH * THERMAL_SENSOR_HEIGHT)

static pthread_mutex_t g_shading_lock = PTHREAD_MUTEX_INITIALIZER;
static FFC_TEST_CB g_shading_calibrate;
static INT16 *g_shading_correction;
static UINT16 *g_shading_base;
static int g_shading_enabled; /* Optional, default off. */
static int g_shading_dirty;
static int g_shading_merged;
static int g_shading_tdc;
static int g_shading_input_nuc, g_shading_input_tdc, g_shading_input_dynamic;
static UINT32 g_shading_generation;
static UINT32 g_table_generation;
static int g_upstream_valid, g_upstream_nuc, g_upstream_tdc;
static UINT32 g_upstream_nuc_generation, g_upstream_tdc_generation;
static UINT32 g_upstream_calibration_generation;
static int g_refresh_required;

void ffc_set_upstream_state(int nuc_enabled, int tdc_enabled,
    UINT32 nuc_generation, UINT32 tdc_generation, UINT32 calibration_generation)
{
    pthread_mutex_lock(&g_shading_lock);
    if (g_upstream_valid && (g_upstream_nuc != nuc_enabled ||
        g_upstream_tdc != tdc_enabled ||
        (nuc_enabled && g_upstream_nuc_generation != nuc_generation) ||
        (tdc_enabled && g_upstream_tdc_generation != tdc_generation) ||
        g_upstream_calibration_generation != calibration_generation)) {
        if (g_ffc.table_ready) {
            printf("FFC reference invalidated: upstream correction changed\n");
            ++g_table_generation;
            g_refresh_required = 1;
        }
        g_ffc.table_ready = 0;
        g_shading_merged = 0;
        g_shading_dirty = 1;
    }
    g_upstream_valid = 1;
    g_upstream_nuc = nuc_enabled;
    g_upstream_tdc = tdc_enabled;
    g_upstream_nuc_generation = nuc_generation;
    g_upstream_tdc_generation = tdc_generation;
    g_upstream_calibration_generation = calibration_generation;
    pthread_mutex_unlock(&g_shading_lock);
}

int ffc_refresh_is_required(void)
{
    int required;
    pthread_mutex_lock(&g_shading_lock);
    required = g_refresh_required;
    pthread_mutex_unlock(&g_shading_lock);
    return required;
}

static UINT32 ffc_shading_checksum(const INT16 *table)
{
    size_t i;
    UINT32 hash = 2166136261U;
    for (i = 0; i < FFC_SHADING_PIXELS; ++i) {
        UINT16 v = (UINT16)table[i];
        hash = (hash ^ (v & 255U)) * 16777619U;
        hash = (hash ^ (v >> 8)) * 16777619U;
    }
    return hash;
}

/* Header and payload are native little-endian on the supported target. */
static int ffc_shading_file(INT16 *table, int save, int *tdc_enabled)
{
    UINT32 header[8] = {0x53434646U, 1U, THERMAL_SENSOR,
        THERMAL_SENSOR_WIDTH, THERMAL_SENSOR_HEIGHT, NUC_GAIN_SHIFT, 0U, 0U};
    UINT32 stored[8];
    FILE *fp;
    int ok;
    if (save && mkdir(FFC_SHADING_DIR, 0777) != 0 && errno != EEXIST) return -1;
    fp = fopen(save ? FFC_SHADING_NEW : FFC_SHADING_PATH, save ? "wb" : "rb");
    if (!fp) return -1;
    if (save) {
        header[6] = *tdc_enabled ? 1U : 0U;
        header[7] = ffc_shading_checksum(table) ^ header[6];
        ok = fwrite(header, sizeof(header), 1, fp) == 1 &&
            fwrite(table, sizeof(*table), FFC_SHADING_PIXELS, fp) == FFC_SHADING_PIXELS;
        if (fflush(fp) != 0) ok = 0;
#if defined(__LINUX)
        if (ok && fsync(fileno(fp)) != 0) ok = 0;
#endif
    } else {
        ok = fread(stored, sizeof(stored), 1, fp) == 1 &&
            memcmp(stored, header, 6U * sizeof(UINT32)) == 0 && stored[6] <= 1U &&
            fread(table, sizeof(*table), FFC_SHADING_PIXELS, fp) == FFC_SHADING_PIXELS &&
            fgetc(fp) == EOF && !ferror(fp) &&
            stored[7] == (ffc_shading_checksum(table) ^ stored[6]);
        if (ok) *tdc_enabled = (int)stored[6];
    }
    if (fclose(fp) != 0) ok = 0;
    if (save) {
        if (ok && rename(FFC_SHADING_NEW, FFC_SHADING_PATH) != 0) ok = 0;
        if (!ok) remove(FFC_SHADING_NEW);
    }
    return ok ? 0 : -1;
}

void ffc_bind_shading_callback(FFC_TEST_CB calibrate)
{
    g_shading_calibrate = calibrate;
}

void ffc_set_shading_enabled(int enabled)
{
    pthread_mutex_lock(&g_shading_lock);
    if (g_shading_enabled != (enabled ? 1 : 0)) ++g_shading_generation;
    g_shading_enabled = enabled ? 1 : 0;
    g_shading_dirty = 1;
    printf("FFC shading %s, table=%s (requires FFC on)\n",
        g_shading_enabled ? "on" : "off", g_shading_correction ? "ready" : "not ready");
    pthread_mutex_unlock(&g_shading_lock);
}

int ffc_is_shading_enabled(void)
{
    int enabled;
    pthread_mutex_lock(&g_shading_lock);
    enabled = g_shading_enabled;
    pthread_mutex_unlock(&g_shading_lock);
    return enabled;
}

int ffc_shading_is_ready(void)
{
    int ready;
    pthread_mutex_lock(&g_shading_lock);
    ready = g_shading_correction != NULL;
    pthread_mutex_unlock(&g_shading_lock);
    return ready;
}

int ffc_shading_is_active(void)
{
    int active;
    pthread_mutex_lock(&g_shading_lock);
    active = g_ffc.enabled && g_ffc.table_ready && g_shading_merged;
    pthread_mutex_unlock(&g_shading_lock);
    return active;
}

void ffc_set_shading_input_mode(int nuc_enabled, int tdc_enabled, int dynamic_nuc)
{
    pthread_mutex_lock(&g_shading_lock);
    if (g_shading_input_nuc != nuc_enabled || g_shading_input_tdc != tdc_enabled ||
        g_shading_input_dynamic != dynamic_nuc) {
        g_shading_input_nuc = nuc_enabled;
        g_shading_input_tdc = tdc_enabled;
        g_shading_input_dynamic = dynamic_nuc;
        g_shading_dirty = 1;
        ++g_shading_generation;
        if (g_shading_enabled && (!nuc_enabled || dynamic_nuc || tdc_enabled != g_shading_tdc))
            printf("FFC shading suspended: NUC/TDC mode differs from calibration\n");
    }
    pthread_mutex_unlock(&g_shading_lock);
}

UINT32 ffc_table_generation(void)
{
    UINT32 value;
    pthread_mutex_lock(&g_shading_lock);
    value = g_table_generation;
    pthread_mutex_unlock(&g_shading_lock);
    return value;
}

UINT32 ffc_shading_generation(void)
{
    UINT32 generation;
    pthread_mutex_lock(&g_shading_lock);
    generation = g_shading_generation;
    pthread_mutex_unlock(&g_shading_lock);
    return generation;
}

int ffc_shading_calibrate(const UINT16 *open_raw, const UINT16 *shutter_raw,
    int tdc_enabled)
{
    INT16 *table = malloc(FFC_SHADING_PIXELS * sizeof(*table));
    if (!table) return -1;
    if (ffc_shading_build(open_raw, shutter_raw, table, FFC_SHADING_PIXELS) != 0 ||
        !g_shading_base || ffc_shading_file(table, 1, &tdc_enabled) != 0) {
        printf("FFC shading calibration/save failed; previous table retained\n");
        free(table);
        return -1;
    }
    pthread_mutex_lock(&g_shading_lock);
    free(g_shading_correction);
    g_shading_correction = table;
    g_shading_tdc = tdc_enabled ? 1 : 0;
    g_shading_dirty = 1;
    ++g_shading_generation;
    pthread_mutex_unlock(&g_shading_lock);
    printf("FFC shading calibration saved: %s\n", FFC_SHADING_PATH);
    return 0;
}

/* Only the RAW worker writes the DMA table. CFG only marks it dirty.
 * Always rebuild from the unmodified FFC base: never add C repeatedly. */
static void ffc_refresh_shading(void)
{
    size_t i;
    int merge;
    pthread_mutex_lock(&g_shading_lock);
    if (!g_shading_dirty || !g_shading_base || !g_ffc.table_ready) {
        pthread_mutex_unlock(&g_shading_lock);
        return;
    }
    merge = g_shading_enabled && g_shading_correction && g_shading_input_nuc &&
        !g_shading_input_dynamic && g_shading_input_tdc == g_shading_tdc;
    if (merge) {
        for (i = 0; i < FFC_SHADING_PIXELS; ++i) {
            INT32 b = (INT16)g_shading_base[i] + (INT32)g_shading_correction[i];
            if (b < -32768 || b > 32767) {
                printf("FFC shading combined B out of range; using ordinary FFC\n");
                merge = 0;
                break;
            }
        }
    }
    memcpy((void *)g_ffc.b_va, g_shading_base, g_ffc.table_size);
    if (merge) {
        UINT16 *b = (UINT16 *)g_ffc.b_va;
        for (i = 0; i < FFC_SHADING_PIXELS; ++i)
            b[i] = (UINT16)(INT16)((INT16)g_shading_base[i] + (INT32)g_shading_correction[i]);
    }
    hd_common_mem_flush_cache((void *)g_ffc.b_va, g_ffc.table_size);
    g_shading_dirty = 0;
    if (g_shading_merged != merge)
        printf("FFC shading compensation %s\n", merge ? "applied" : "inactive");
    g_shading_merged = merge;
    pthread_mutex_unlock(&g_shading_lock);
}

static int ffc_read_option(void)
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

static UINT16 ffc_store_b(INT32 value)
{
	if (value < -32768) {
		value = -32768;
	} else if (value > 32767) {
		value = 32767;
	}

	return (UINT16)(short)value;
}

static UINT16 ffc_clip_output(long long value)
{
	if (value <= 0) {
		return 0;
	}
	if (value > 65535) {
		return 65535;
	}

	return (UINT16)value;
}

void ffc_bind_test_callback(FFC_TEST_CB run_test)
{
	g_ffc.run_test = run_test;
}

void ffc_bind_runtime(UINTPTR b_va, UINT32 table_size)
{
	INT16 *loaded = NULL;
	pthread_mutex_lock(&g_shading_lock);
	free(g_shading_base);
	free(g_shading_correction);
	g_shading_base = NULL;
	g_shading_correction = NULL;
	g_shading_merged = 0;
	g_shading_dirty = 1;
	g_upstream_valid = 0;
	g_refresh_required = 0;
	g_ffc.table_ready = 0;
	if (b_va && table_size == FFC_SHADING_PIXELS * sizeof(UINT16)) {
		g_shading_base = calloc(1, table_size);
		loaded = malloc(table_size);
		if (g_shading_base && loaded && ffc_shading_file(loaded, 0, &g_shading_tdc) == 0) {
			g_shading_correction = loaded;
			printf("FFC shading table loaded: %s\n", FFC_SHADING_PATH);
		} else {
			free(loaded);
			printf("FFC shading table missing/invalid; ordinary FFC remains available\n");
		}
	}
	g_ffc.b_va = b_va;
	g_ffc.table_size = table_size;
	pthread_mutex_unlock(&g_shading_lock);
}

int ffc_init_table(void)
{
	if (g_ffc.table_size == 0 || g_ffc.b_va == 0) {
		printf("FFC runtime buffer is not ready\n");
		return -1;
	}

	memset((void *)g_ffc.b_va, 0, g_ffc.table_size);

	hd_common_mem_flush_cache((void *)g_ffc.b_va, g_ffc.table_size);

	g_ffc.table_ready = 0;
	pthread_mutex_lock(&g_shading_lock);
	g_shading_merged = 0;
	++g_table_generation;
	g_refresh_required = 0;
	g_shading_dirty = 1;
	pthread_mutex_unlock(&g_shading_lock);
	return 0;
}

int ffc_update_table_from_raw(UINTPTR raw_va,
			      UINT32 lineoffset,
			      UINT32 active_x,
			      UINT32 active_y,
			      UINT32 active_w,
			      UINT32 active_h)
{
	UINT16 *b_tbl;
	UINT32 raw_width;
	UINT32 i, j;
	UINT64 sum = 0;
	UINT32 mean;

	if (raw_va == 0 || lineoffset == 0) {
		printf("FFC raw buffer is invalid\n");
		return -1;
	}
	if (g_ffc.table_size == 0 || g_ffc.b_va == 0) {
		printf("FFC runtime buffer is not ready\n");
		return -1;
	}

	raw_width = lineoffset / 2;
	if ((active_y + active_h) * raw_width * 2 > g_ffc.table_size) {
		printf("FFC active area over table size\n");
		return -1;
	}

	for (i = 0; i < active_h; i++) {
		UINT16 *row = (UINT16 *)(raw_va + lineoffset * (i + active_y));

		for (j = 0; j < active_w; j++) {
			sum += row[active_x + j];
		}
	}

	mean = (UINT32)(sum / active_w / active_h);
	printf("FFC dark frame mean=%u\n", mean);

	memset((void *)g_ffc.b_va, 0, g_ffc.table_size);
	b_tbl = (UINT16 *)g_ffc.b_va;
	for (i = 0; i < active_h; i++) {
		UINT16 *row = (UINT16 *)(raw_va + lineoffset * (i + active_y));
		UINT32 table_y = i + active_y;

		for (j = 0; j < active_w; j++) {
			UINT32 table_x = j + active_x;
			INT32 offset = (INT32)row[active_x + j] - (INT32)mean;

			b_tbl[table_y * raw_width + table_x] = ffc_store_b(offset);
		}
	}

	hd_common_mem_flush_cache((void *)g_ffc.b_va, g_ffc.table_size);
	g_ffc.table_ready = 1;
    pthread_mutex_lock(&g_shading_lock);
    ++g_table_generation;
    g_refresh_required = 0;
    pthread_mutex_unlock(&g_shading_lock);
	if (g_shading_base) {
		memcpy(g_shading_base, b_tbl, g_ffc.table_size);
		pthread_mutex_lock(&g_shading_lock);
		g_shading_dirty = 1;
		pthread_mutex_unlock(&g_shading_lock);
		ffc_refresh_shading();
	}
	printf("FFC table updated\n");
	return 0;
}

void ffc_set_enabled(int enable)
{
	g_ffc.enabled = enable ? 1 : 0;
	printf("FFC %s\n", g_ffc.enabled ? "on" : "off");
}

int ffc_is_enabled(void)
{
	return g_ffc.enabled;
}

void ffc_set_startup_enabled(int enable)
{
	g_ffc.startup_enabled = enable ? 1 : 0;
}

int ffc_is_startup_enabled(void)
{
	return g_ffc.startup_enabled;
}

void ffc_set_shutter_interval_sec(UINT32 interval_sec)
{
	if (interval_sec > INT_MAX) {
		printf("Shutter interval exceeds INT_MAX sec\n");
		return;
	}
	g_ffc.shutter_interval_sec = interval_sec;
	printf("Shutter interval=%u sec\n",
	       g_ffc.shutter_interval_sec);
}

UINT32 ffc_get_shutter_interval_sec(void)
{
	return g_ffc.shutter_interval_sec;
}

int ffc_table_is_ready(void)
{
	return g_ffc.table_ready;
}

int ffc_compare_gfx_samples(UINTPTR input_va,
			    UINTPTR k_va,
			    UINTPTR output_va,
			    UINT32 lineoffset,
			    UINT32 active_x,
			    UINT32 active_y,
			    UINT32 active_w,
			    UINT32 active_h,
			    UINT32 sample_count)
{
	UINT16 *input;
	UINT16 *k_tbl;
	UINT16 *b_tbl;
	UINT16 *output;
	UINT32 raw_width;
	UINT32 active_pixels;
	UINT32 sample;
	UINT32 pass_count = 0;

	if (input_va == 0 || k_va == 0 || output_va == 0 ||
	    lineoffset == 0 || (lineoffset & 1U) != 0 ||
	    active_w == 0 || active_h == 0 || sample_count == 0) {
		printf("FFC GFX sample buffer or layout is invalid\n");
		return -1;
	}
	if (g_ffc.b_va == 0 || g_ffc.table_size == 0 ||
	    !g_ffc.table_ready) {
		printf("FFC table is not ready for GFX sample compare\n");
		return -1;
	}

	raw_width = lineoffset / sizeof(UINT16);
	if (active_x >= raw_width || active_w > raw_width - active_x ||
	    (active_y + active_h) * lineoffset > g_ffc.table_size) {
		printf("FFC GFX sample active area is invalid\n");
		return -1;
	}

	active_pixels = active_w * active_h;
	if (sample_count > active_pixels) {
		sample_count = active_pixels;
	}

	input = (UINT16 *)input_va;
	k_tbl = (UINT16 *)k_va;
	b_tbl = (UINT16 *)g_ffc.b_va;
	output = (UINT16 *)output_va;

	printf("FFC GFX test samples (first %u active pixels)\n", sample_count);
	printf(" idx  (x,y)  avg_input  K(Q8)  B(s16)  expected  actual_gfx  diff  result\n");
	for (sample = 0; sample < sample_count; sample++) {
		UINT32 x = active_x + sample % active_w;
		UINT32 y = active_y + sample / active_w;
		UINT32 pixel = y * raw_width + x;
		UINT16 input_value = input[pixel];
		UINT16 k_value = k_tbl[pixel];
		INT16 b_value = (INT16)b_tbl[pixel];
		long long scaled = (long long)k_value *
			((long long)input_value - (long long)b_value);
		UINT16 expected = ffc_clip_output(
			scaled > 0 ? scaled >> FFC_GAIN_SHIFT : scaled);
		UINT16 actual = output[pixel];
		INT32 diff = (INT32)actual - (INT32)expected;
		int pass = diff == 0;

		if (pass) {
			pass_count++;
		}
		printf(" %3u  (%u,%u)  %9u  %5u  %6d  %8u  %10u  %4d  %s\n",
		       sample, x, y, input_value, k_value, b_value,
		       expected, actual, diff, pass ? "PASS" : "FAIL");
	}

	printf("FFC GFX compare: %u/%u PASS\n", pass_count, sample_count);
	return pass_count == sample_count ? 0 : -1;
}

int ffc_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("FFC status\n");
		printf(" 1. Run FFC test\n");
		printf(" 2. Calibrate blackbody / shutter shading (32 frames each)\n");
		printf("    Shading requested=%s, applied=%s, table=%s; enable in CFG Setting -> Shutter / FFC\n",
		       ffc_is_shading_enabled() ? "on" : "off",
		       ffc_shading_is_active() ? "yes" : "no",
		       ffc_shading_is_ready() ? "ready" : "not ready");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: %s, table=%s, shutter interval=%u sec\n",
		       g_ffc.enabled ? "on" : "off",
		       g_ffc.table_ready ? "ready" : "not ready",
		       g_ffc.shutter_interval_sec);
		printf("Settings: CFG Setting -> Shutter / FFC setting\n");

		option = ffc_read_option();
		switch (option) {
		case 2:
			printf("Fill the entire view with a uniform blackbody. Keep camera/NUC/TDC stable.\n");
			printf("Capture blackbody, then automatically close shutter and capture reference.\n");
			printf("Enter 1 to calibrate and save (0 cancels).\n");
			if (ffc_read_option() == 1) {
				if (!g_shading_calibrate)
					printf("Shading calibration unavailable: start Liveview first\n");
				else
					printf("Shading calibration %s\n", g_shading_calibrate() == 0 ? "PASS" : "FAIL");
			}
			break;
		case 1:
			if (g_ffc.run_test == 0) {
				printf("Run FFC test unavailable\n");
				break;
			}
			printf("Run FFC test requested\n");
			printf("Run FFC test %s\n",
			       g_ffc.run_test() == 0 ? "PASS" : "FAIL");
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

	return 0;
}

HD_RESULT ffc_apply(HD_VIDEO_FRAME *in_frame,
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

	if (!g_ffc.table_ready) return HD_ERR_NG;
	ffc_refresh_shading();
	memcpy(out_frame, in_frame, sizeof(HD_VIDEO_FRAME));

	param.command = VENDOR_GRPH_CMD_Non_uni_correction;
	param.format = VENDOR_GFX_GRPH_FORMAT_16BITS;
	param.property[0].en = 1;
	param.property[0].id = VENDOR_GFX_GRPH_PROPERTY_ID_NORMAL;
	param.property[0].data.property = FFC_GAIN_SHIFT;

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
		printf("FFC vendor_gfx_grph_trigger fail=%d\n", ret);
	}

	out_frame->phy_addr[0] = out_pa;
	out_frame->blk = out_blk;

	return ret;
}
