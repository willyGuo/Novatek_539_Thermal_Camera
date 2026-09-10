#include "menu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "../Correction/BPR/bpr.h"
#include "../Correction/FFC/ffc.h"
#include "../Correction/NUC/nuc.h"
#include "../Correction/TDC/tdc.h"
#include "../Config/app_config.h"
#include "../Device/FOS/fos.h"
#include "../Enhancement/AGC/agc_mode.h"
#include "../Enhancement/ISP/videoproc_isp.h"
#include "../Diagnostics/diagnostics_menu.h"
#include "../app.h"
#include "liveview.h"

#define MENU_OPTION_REFRESH (-2)

static int menu_read_option(void)
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

//Philip ============================================================			
extern int clahe_clip_limit_menu_run(void);
//Philip ============================================================	

int menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Tiffany Thermal Firmware %s\n", app_get_version());
		printf(" 1. Liveview\n");
		printf(" 2. CFG Setting\n");
		printf(" 3. BPR\n");
		printf(" 4. NUC\n");
		printf(" 5. FFC\n");
		printf(" 6. TDC\n");
		printf(" 7. AGC\n");
		printf(" 8. FOS\n");
		printf(" 9. VideoProc / ISP\n");
		printf("10. Image Pipeline Diagnostics\n");
		printf("11. CLAHE Clip Limit Setting\n");
		printf(" q. Quit\n");
		printf("----------------------------------------\n");

		option = menu_read_option();
		switch (option) {
		case 1:
			ret = liveview_menu_run();
			break;
		case 2:
			ret = app_config_menu_run();
			break;
		case 3:
			ret = bpr_menu_run();
			break;
		case 4:
			ret = nuc_menu_run();
			break;
		case 5:
			ret = ffc_menu_run();
			break;
		case 6:
			ret = tdc_menu_run();
			break;
		case 7:
			ret = agc_menu_run();
			break;
		case 8:
			ret = fos_menu_run();
			break;
		case 9:
			ret = videoproc_isp_menu_run();
			break;
		case 10:
			ret = diagnostics_menu_run();
			break;
//Philip ============================================================			
		case 11 :
		    ret = clahe_clip_limit_menu_run();
			break;
//Philip ============================================================				
		case MENU_OPTION_REFRESH:
			break;

		case -1:
			running = 0;
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;
}
