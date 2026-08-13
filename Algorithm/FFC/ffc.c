#include "ffc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor_gfx.h"

#define SHUTTER_DEFAULT_INTERVAL_SEC 60U
#define FFC_GAIN_SHIFT 8U
#define MENU_OPTION_REFRESH (-2)

typedef struct {
	UINTPTR b_va;
	UINT32 table_size;
	UINT32 shutter_interval_sec;
	int enabled;
	int table_ready;
} FFC_CONTEXT;

static FFC_CONTEXT g_ffc = {
	.shutter_interval_sec = SHUTTER_DEFAULT_INTERVAL_SEC,
	.enabled = 1,
};

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

void ffc_bind_runtime(UINTPTR b_va, UINT32 table_size)
{
	g_ffc.b_va = b_va;
	g_ffc.table_size = table_size;
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

void ffc_set_shutter_interval_sec(UINT32 interval_sec)
{
	if (interval_sec == 0) {
		printf("Shutter interval must be greater than 0 sec\n");
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

HD_RESULT ffc_accumulate_frame(UINTPTR raw_pa,
			       UINTPTR sum_pa,
			       UINTPTR out_pa,
			       UINT32 raw_width,
			       UINT32 raw_height,
			       UINT32 shift)
{
	VENDOR_GFX_GRPH_TRIGGER_PARAM param = {0};
	HD_RESULT ret;

	param.command = VENDOR_GFX_GRPH_CMD_PLUS_SHF;
	param.format = VENDOR_GFX_GRPH_FORMAT_16BITS;
	param.property[0].en = 1;
	param.property[0].id = VENDOR_GFX_GRPH_PROPERTY_ID_NORMAL;
	param.property[0].data.property = VENDOR_GFX_GRPH_PLUS_PROPTY(shift);

	param.images[0].img_id = VENDOR_GFX_GRPH_IMG_ID_A;
	param.images[0].dram_addr = raw_pa;
	param.images[0].lineoffset = raw_width * 2;
	param.images[0].width = raw_width * 2;
	param.images[0].height = raw_height;

	param.images[1].img_id = VENDOR_GFX_GRPH_IMG_ID_B;
	param.images[1].dram_addr = sum_pa;
	param.images[1].lineoffset = raw_width * 2;

	param.images[2].img_id = VENDOR_GFX_GRPH_IMG_ID_C;
	param.images[2].dram_addr = out_pa;
	param.images[2].lineoffset = raw_width * 2;

	ret = vendor_gfx_grph_trigger(1, &param);
	if (ret != HD_OK) {
		printf("FFC accumulate frame fail=%d\n", ret);
	}

	return ret;
}

int ffc_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("FFC status\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: %s, table=%s, shutter interval=%u sec\n",
		       g_ffc.enabled ? "on" : "off",
		       g_ffc.table_ready ? "ready" : "not ready",
		       g_ffc.shutter_interval_sec);
		printf("Settings: CFG Setting -> Shutter / FFC setting\n");

		option = ffc_read_option();
		switch (option) {
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
