#include "gamma.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Config/hardware_config.h"
#include "vendor_isp.h"

#define GAMMA_BIN_PATH "/mnt/sd/thermal_op/gamma.bin"
#define GAMMA_LUT_SIZE 129U
#define GAMMA_AUTO_SET_COUNT 5U
#define MENU_OPTION_REFRESH (-2)

typedef struct {
	UINT32 manual_lut[GAMMA_LUT_SIZE];
	UINT32 auto_set_lut[GAMMA_AUTO_SET_COUNT][GAMMA_LUT_SIZE];
} GAMMA_BIN_TABLE;

static int g_gamma_default_ready;
static IQT_GAMMA_PARAM g_gamma_default;
static int g_gamma_enabled = 1;
static UINT32 g_gamma_mode;
static UINT32 g_gamma_auto_sel = 2;
static int g_gamma_load_bin;

static int gamma_read_option(void)
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

static int gamma_read_bin(GAMMA_BIN_TABLE *table)
{
	FILE *fp;
	UINT32 read_size;

	fp = fopen(GAMMA_BIN_PATH, "rb");
	if (fp == NULL) {
		printf("open Gamma bin (%s) fail\n", GAMMA_BIN_PATH);
		return -1;
	}

	read_size = (UINT32)fread(table, 1, sizeof(*table), fp);
	fclose(fp);
	if (read_size != sizeof(*table)) {
		printf("read Gamma bin size error, read=%u expect=%u\n",
		       read_size, (UINT32)sizeof(*table));
		return -1;
	}

	return 0;
}

static void gamma_copy_bin_to_iq(const GAMMA_BIN_TABLE *table,
				 IQT_GAMMA_PARAM *gamma)
{
	memcpy(gamma->gamma.manual_lut, table->manual_lut,
	       sizeof(table->manual_lut));
	memcpy(gamma->gamma.auto_set0_lut, table->auto_set_lut[0],
	       sizeof(table->auto_set_lut[0]));
	memcpy(gamma->gamma.auto_set1_lut, table->auto_set_lut[1],
	       sizeof(table->auto_set_lut[1]));
	memcpy(gamma->gamma.auto_set2_lut, table->auto_set_lut[2],
	       sizeof(table->auto_set_lut[2]));
	memcpy(gamma->gamma.auto_set3_lut, table->auto_set_lut[3],
	       sizeof(table->auto_set_lut[3]));
	memcpy(gamma->gamma.auto_set4_lut, table->auto_set_lut[4],
	       sizeof(table->auto_set_lut[4]));
}

HD_RESULT gamma_init_from_isp(void)
{
	HD_RESULT ret;

	memset(&g_gamma_default, 0, sizeof(g_gamma_default));
	g_gamma_default.id = g_thermal_pipeline_id;
	ret = vendor_isp_get_iq(IQT_ITEM_GAMMA_PARAM, &g_gamma_default);
	if (ret != HD_OK) {
		g_gamma_default_ready = 0;
		printf("get default Gamma from ISP failed=%d\n", ret);
		return ret;
	}

	g_gamma_default_ready = 1;
	g_gamma_enabled = g_gamma_default.gamma.enable ? 1 : 0;
	g_gamma_mode = g_gamma_default.gamma.mode;
	g_gamma_auto_sel = g_gamma_default.gamma.auto_sel;
	printf("Default Gamma loaded from ISP\n");
	return HD_OK;
}

HD_RESULT gamma_apply(void)
{
	GAMMA_BIN_TABLE table;
	IQT_GAMMA_PARAM gamma;
	HD_RESULT ret;
	int bin_ret = 0;

	if (!g_gamma_default_ready) {
		printf("Default Gamma is not ready\n");
		return HD_ERR_NG;
	}

	memcpy(&gamma, &g_gamma_default, sizeof(gamma));
	gamma.id = g_thermal_pipeline_id;
	gamma.gamma.enable = g_gamma_enabled;
	gamma.gamma.mode = g_gamma_mode;
	gamma.gamma.auto_sel = g_gamma_auto_sel;

	if (g_gamma_load_bin) {
		bin_ret = gamma_read_bin(&table);
		if (bin_ret == 0) {
			gamma_copy_bin_to_iq(&table, &gamma);
		} else {
			printf("Keep ISP cfg default Gamma LUT\n");
		}
	}

	ret = vendor_isp_set_iq(IQT_ITEM_GAMMA_PARAM, &gamma);
	if (ret != HD_OK) {
		printf("apply Gamma failed=%d\n", ret);
		return ret;
	}

	printf("Gamma applied: %s, mode=%u, auto_sel=%u, LUT=%s\n",
	       g_gamma_enabled ? "on" : "off",
	       g_gamma_mode,
	       g_gamma_auto_sel,
	       g_gamma_load_bin && bin_ret == 0 ? "bin" : "ISP cfg");

	return bin_ret == 0 ? HD_OK : HD_ERR_NG;
}

void gamma_set_enabled(int enable)
{
	g_gamma_enabled = enable ? 1 : 0;
}

int gamma_is_enabled(void)
{
	return g_gamma_enabled;
}

void gamma_set_mode(UINT32 mode)
{
	g_gamma_mode = mode;
}

UINT32 gamma_get_mode(void)
{
	return g_gamma_mode;
}

void gamma_set_auto_sel(UINT32 auto_sel)
{
	g_gamma_auto_sel = auto_sel;
}

UINT32 gamma_get_auto_sel(void)
{
	return g_gamma_auto_sel;
}

void gamma_set_load_bin(int enable)
{
	g_gamma_load_bin = enable ? 1 : 0;
}

int gamma_get_load_bin(void)
{
	return g_gamma_load_bin;
}

int gamma_menu_run(void)
{
	int option;
	int running = 1;

	while (running) {
		printf("----------------------------------------\n");
		printf("Gamma status\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: Gamma=%s, Gamma bin=%s, mode=%u, auto_sel=%u\n",
		       g_gamma_enabled ? "on" : "off",
		       g_gamma_load_bin ? "on" : "off",
		       g_gamma_mode, g_gamma_auto_sel);
		printf("Settings: CFG Setting -> Gamma setting\n");

		option = gamma_read_option();
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
