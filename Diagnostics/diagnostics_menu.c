#include "diagnostics_menu.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "../Pipeline/liveview.h"
#include "ISP/isp_ob_runtime_diagnostic.h"
#include "ISP/isp_raw_to_y_step_diagnostic.h"

#define DIAGNOSTICS_MENU_OPTION_REFRESH (-2)

static int diagnostics_menu_read_option(void)
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
		return DIAGNOSTICS_MENU_OPTION_REFRESH;
	}
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') {
		end++;
	}
	if (*end != '\0' || value > INT_MAX) {
		printf("Invalid option\n");
		return DIAGNOSTICS_MENU_OPTION_REFRESH;
	}

	return (int)value;
}

int diagnostics_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Image Pipeline Diagnostics\n");
		printf(" 1. Run Full RAW14 0..16383 Step Test\n");
		printf(" 2. Run RAW12 Numeric 0..4095 Step Test\n");
		printf(" 3. Run Fine RAW14 6000..9000 Step Test\n");
		printf(" 4. Show ISP OB runtime parameters\n");
		printf(" 5. Run Full RAW14 Step Test with ISP OB forced to zero\n");
		printf(" 6. Show last Step Test result in this run\n");
		printf(" 7. Run Timed Snapshot (every 1 minute, 32 snapshots)\n");
		printf(" 8. Show Timed Snapshot status\n");
		printf(" 9. Stop Timed Snapshot\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("Liveview must be running for Step Tests and Timed Snapshot.\n");
		printf("Step Tests send one test-pattern frame; Timed Snapshot saves live RAW.\n");

		option = diagnostics_menu_read_option();
		switch (option) {
		case 1:
			ret = liveview_run_isp_raw_to_y_step_diagnostic();
			break;

		case 2:
			ret = liveview_run_isp_raw12_to_y_step_diagnostic();
			break;

		case 3:
			ret = liveview_run_isp_raw14_fine_step_diagnostic();
			break;

		case 4:
			ret = isp_ob_runtime_diagnostic_show();
			break;

		case 5:
			ret = isp_ob_runtime_diagnostic_run_zero_offset(
				liveview_run_isp_raw_to_y_step_diagnostic);
			break;

		case 6:
			isp_raw_to_y_step_diagnostic_print_last_result();
			break;

		case 7:
			ret = liveview_start_timed_snapshot();
			break;

		case 8:
			liveview_show_timed_snapshot_status();
			break;

		case 9:
			liveview_stop_timed_snapshot();
			liveview_show_timed_snapshot_status();
			break;

		case 0:
		case -1:
			running = 0;
			break;

		case DIAGNOSTICS_MENU_OPTION_REFRESH:
			break;

		default:
			printf("Invalid option\n");
			break;
		}
	}

	return ret;
}
