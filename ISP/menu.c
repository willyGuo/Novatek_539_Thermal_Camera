#include "menu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "../Config/app_config.h"
#include "../Pipeline/liveview.h"
#include "Pseudo/pseudo.h"
#include "videoproc/videoproc_isp.h"

#define MENU_OPTION_REFRESH (-2)

static int isp_read_option(void)
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

static int isp_prompt_value(const char *key, int maximum)
{
	int value = isp_read_option();

	if (value == -1 || value == MENU_OPTION_REFRESH) {
		return 0;
	}
	if (value < 0 || value > maximum) {
		printf("Invalid value, enter 0..%d; keep pending CFG\n", maximum);
		return -1;
	}
	return app_config_update_int(key, value);
}

int isp_menu_run(void)
{
	int mirror;
	int pseudo;
	int ret = 0;

	for (;;) {
		if (app_config_get_int("liveview_mirror", &mirror) != 0 ||
		    app_config_get_int("pseudo_color_mode", &pseudo) != 0) {
			printf("read pending ISP CFG fail\n");
			return -1;
		}
		printf("----------------------------------------\n");
		printf("ISP setting\n");
		printf(" 1. videoproc_isp\n");
		printf(" 2. Horizontal mirror\n");
		printf(" 3. Pseudo color\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Horizontal mirror: runtime=%s, pending CFG=%s\n",
		       liveview_get_mirror() ? "on" : "off", mirror ? "on" : "off");
		printf("Pseudo color: runtime=%s, pending CFG=%s\n",
		       pseudo_mode_name(pseudo_get_mode()), pseudo_mode_name(pseudo));
		printf("Pseudo color applies on next display frame; other settings require Liveview Restart\n");

		switch (isp_read_option()) {
		case 1:
			ret = videoproc_isp_menu_run();
			break;
		case 2:
			printf("Horizontal mirror (0: off, 1: on, pending CFG: %d)\n", mirror);
			ret = isp_prompt_value("liveview_mirror", 1);
			break;
		case 3:
			printf("0: Black/White, 1: White/Black, 2: Color (Jet), 3: Iron\n");
			printf("Pseudo color mode (pending CFG: %d)\n", pseudo);
			ret = isp_prompt_value("pseudo_color_mode", 3);
			break;
		case 0:
		case -1:
			return ret;
		case MENU_OPTION_REFRESH:
			break;
		default:
			printf("Invalid option\n");
			break;
		}
	}
}
