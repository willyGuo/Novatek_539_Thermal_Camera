#include "gamma.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../Config/hardware_config.h"
#include "vendor_isp.h"

#define MENU_OPTION_REFRESH (-2)

static int g_gamma_ready;
static IQT_GAMMA_PARAM g_gamma_runtime;

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

HD_RESULT gamma_init_from_isp(void)
{
	IQT_GAMMA_PARAM gamma;
	HD_RESULT ret;

	memset(&gamma, 0, sizeof(gamma));
	gamma.id = g_thermal_pipeline_id;
	ret = vendor_isp_get_iq(IQT_ITEM_GAMMA_PARAM, &gamma);
	if (ret != HD_OK) {
		g_gamma_ready = 0;
		printf("get Gamma from loaded ISP cfg failed=%d, path=%s\n",
		       ret, THERMAL_ISP_CONFIG_PATH);
		return ret;
	}

	g_gamma_runtime = gamma;
	g_gamma_ready = 1;
	printf("Gamma loaded from ISP cfg: %s, enable=%s\n",
	       THERMAL_ISP_CONFIG_PATH,
	       g_gamma_runtime.gamma.enable ? "on" : "off");
	return HD_OK;
}

void gamma_reset(void)
{
	g_gamma_ready = 0;
	memset(&g_gamma_runtime, 0, sizeof(g_gamma_runtime));
}

HD_RESULT gamma_set_enabled(int enable)
{
	IQT_GAMMA_PARAM gamma;
	HD_RESULT ret;

	if (!g_gamma_ready) {
		printf("Gamma is unavailable; ISP cfg was not loaded from %s\n",
		       THERMAL_ISP_CONFIG_PATH);
		return HD_ERR_NG;
	}

	gamma = g_gamma_runtime;
	gamma.id = g_thermal_pipeline_id;
	gamma.gamma.enable = enable ? 1 : 0;
	ret = vendor_isp_set_iq(IQT_ITEM_GAMMA_PARAM, &gamma);
	if (ret != HD_OK) {
		printf("set Gamma runtime enable failed=%d\n", ret);
		return ret;
	}

	g_gamma_runtime = gamma;
	printf("Gamma runtime enable=%s; source cfg remains %s\n",
	       g_gamma_runtime.gamma.enable ? "on" : "off",
	       THERMAL_ISP_CONFIG_PATH);
	return HD_OK;
}

int gamma_is_enabled(void)
{
	return g_gamma_ready && g_gamma_runtime.gamma.enable;
}

int gamma_menu_run(void)
{
	int option;
	int value;
	int running = 1;
	int ret = 0;

	if (!g_gamma_ready) {
		printf("Gamma is unavailable; start Liveview with a valid %s first\n",
		       THERMAL_ISP_CONFIG_PATH);
		return -1;
	}

	while (running) {
		printf("----------------------------------------\n");
		printf("Gamma\n");
		printf(" 1. Gamma on/off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("ISP cfg: %s\n", THERMAL_ISP_CONFIG_PATH);
		printf("runtime: Gamma=%s\n",
		       gamma_is_enabled() ? "on" : "off");
		printf("Changes apply to the current ISP runtime only.\n");

		option = gamma_read_option();
		switch (option) {
		case 1:
			printf("Gamma (0: off, 1: on, runtime: %s)\n",
			       gamma_is_enabled() ? "on" : "off");
			value = gamma_read_option();
			if (value == -1 || value == MENU_OPTION_REFRESH) {
				break;
			}
			if (value != 0 && value != 1) {
				printf("Invalid Gamma value, enter 0 or 1\n");
				ret = -1;
				break;
			}
			ret = gamma_set_enabled(value);
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
