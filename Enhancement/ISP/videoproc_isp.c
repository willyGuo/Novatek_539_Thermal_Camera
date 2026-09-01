#include "videoproc_isp.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../Config/app_config.h"
#include "../Gamma/gamma.h"

#define MENU_OPTION_REFRESH (-2)

typedef struct {
	int af_enable;
	int wdr_enable;
	int defog_enable;
	int three_dnr_enable;
} VIDEOPROC_ISP_SETTINGS;

static VIDEOPROC_ISP_SETTINGS g_videoproc_isp_runtime = {
	.af_enable = 1,
	.wdr_enable = 1,
	.defog_enable = 1,
	.three_dnr_enable = 1,
};

void videoproc_isp_set_runtime(int af_enable, int wdr_enable,
				       int defog_enable, int three_dnr_enable)
{
	g_videoproc_isp_runtime.af_enable = af_enable ? 1 : 0;
	g_videoproc_isp_runtime.wdr_enable = wdr_enable ? 1 : 0;
	g_videoproc_isp_runtime.defog_enable = defog_enable ? 1 : 0;
	g_videoproc_isp_runtime.three_dnr_enable = three_dnr_enable ? 1 : 0;
}

int videoproc_isp_is_af_enabled(void)
{
	return g_videoproc_isp_runtime.af_enable;
}

int videoproc_isp_is_wdr_enabled(void)
{
	return g_videoproc_isp_runtime.wdr_enable;
}

int videoproc_isp_is_defog_enabled(void)
{
	return g_videoproc_isp_runtime.defog_enable;
}

int videoproc_isp_is_3dnr_enabled(void)
{
	return g_videoproc_isp_runtime.three_dnr_enable;
}

static int videoproc_isp_read_option(void)
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

static int videoproc_isp_load_pending(VIDEOPROC_ISP_SETTINGS *pending)
{
	if (pending == NULL ||
	    app_config_get_int("videoproc_af_enable", &pending->af_enable) != 0 ||
	    app_config_get_int("videoproc_wdr_enable", &pending->wdr_enable) != 0 ||
	    app_config_get_int("videoproc_defog_enable",
			       &pending->defog_enable) != 0 ||
	    app_config_get_int("videoproc_3dnr_enable",
			       &pending->three_dnr_enable) != 0) {
		printf("read pending VideoProc / ISP CFG fail\n");
		return -1;
	}

	return 0;
}

static int videoproc_isp_prompt_bool(const char *label, const char *key,
				     int current)
{
	int value;

	printf("%s (0: off, 1: on, pending CFG: %s)\n",
	       label, current ? "on" : "off");
	value = videoproc_isp_read_option();
	if (value == -1 || value == MENU_OPTION_REFRESH) {
		return 0;
	}
	if (value != 0 && value != 1) {
		printf("Invalid %s, enter 0 or 1; keep pending CFG %s\n",
		       label, current ? "on" : "off");
		return -1;
	}

	return app_config_update_int(key, value);
}

int videoproc_isp_menu_run(void)
{
	VIDEOPROC_ISP_SETTINGS pending;
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		if (videoproc_isp_load_pending(&pending) != 0) {
			return -1;
		}
		printf("----------------------------------------\n");
		printf("VideoProc / ISP setting\n");
		printf(" 1. AF on/off\n");
		printf(" 2. WDR on/off\n");
		printf(" 3. DEFOG on/off\n");
		printf(" 4. 3DNR + 3DNR statistics on/off\n");
		printf(" 5. All on\n");
		printf(" 6. All off\n");
		printf(" 7. Gamma on/off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: AF=%s, WDR=%s, DEFOG=%s, 3DNR=%s\n",
		       videoproc_isp_is_af_enabled() ? "on" : "off",
		       videoproc_isp_is_wdr_enabled() ? "on" : "off",
		       videoproc_isp_is_defog_enabled() ? "on" : "off",
		       videoproc_isp_is_3dnr_enabled() ? "on" : "off");
		printf("pending CFG: AF=%s, WDR=%s, DEFOG=%s, 3DNR=%s\n",
		       pending.af_enable ? "on" : "off",
		       pending.wdr_enable ? "on" : "off",
		       pending.defog_enable ? "on" : "off",
		       pending.three_dnr_enable ? "on" : "off");
		printf("Changes take effect after Liveview Restart\n");

		option = videoproc_isp_read_option();
		switch (option) {
		case 1:
			ret = videoproc_isp_prompt_bool(
				"AF", "videoproc_af_enable", pending.af_enable);
			break;

		case 2:
			ret = videoproc_isp_prompt_bool(
				"WDR", "videoproc_wdr_enable", pending.wdr_enable);
			break;

		case 3:
			ret = videoproc_isp_prompt_bool(
				"DEFOG", "videoproc_defog_enable",
				pending.defog_enable);
			break;

		case 4:
			ret = videoproc_isp_prompt_bool(
				"3DNR + 3DNR statistics",
				"videoproc_3dnr_enable", pending.three_dnr_enable);
			break;

		case 5:
			ret = app_config_update_videoproc_isp(1, 1, 1, 1);
			break;

		case 6:
			ret = app_config_update_videoproc_isp(0, 0, 0, 0);
			break;

		case 7:
			ret = gamma_menu_run();
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
