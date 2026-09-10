#include "app_config.h"

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

#include "../Correction/BPR/bpr.h"
#include "../Correction/FFC/ffc.h"
#include "../Correction/NUC/nuc.h"
#include "../Correction/TDC/tdc.h"
#include "../Enhancement/AGC/agc_fieldscale.h"
#include "../Enhancement/AGC/agc_histogram_p1_p99.h"
#include "../Enhancement/AGC/agc_min_max_tone_curve.h"
#include "../Enhancement/AGC/agc_mode.h"
#include "../Enhancement/ISP/videoproc_isp.h"
#include "../Pipeline/liveview.h"

#define APP_CONFIG_DIR "/mnt/sd"
#define APP_CONFIG_PATH "/mnt/sd/tiffany_thermal.cfg"
#define APP_CONFIG_TEMP_PATH APP_CONFIG_PATH ".new"
#define APP_CONFIG_SENSOR_TEMP_POINT_COUNT 5U
#define MENU_OPTION_REFRESH (-2)

typedef struct {
	int initialized;
	int boot_load;
	unsigned int fps;
	unsigned int output_mode;
	unsigned int startup_skip_frames;
	unsigned int runtime_skip_frames;
	int nuc_correction;
	unsigned int nuc_sensor_temp_interval_sec;
	float nuc_sensor_temp_points[APP_CONFIG_SENSOR_TEMP_POINT_COUNT];
	int bpr_correction;
	unsigned int bpr_threshold;
	int runtime_bpr_enable;
	unsigned int runtime_bpr_threshold;
	int ffc_correction;
	unsigned int shutter_interval_sec;
	unsigned int shutter_close_wait_ms;
	int tdc_correction;
	unsigned int tdc_interval_sec;
	int agc_histogram_p1_p99_enable;
	int agc_min_max_tone_curve_enable;
	int agc_fieldscale_enable;
	int videoproc_af_enable;
	int videoproc_wdr_enable;
	int videoproc_defog_enable;
	int videoproc_3dnr_enable;
} APP_CONFIG_VALUES;

static APP_CONFIG_VALUES g_app_config;

static int app_config_read_option(void)
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

static char *cfg_trim(char *str)
{
	char *end;

	while (*str == ' ' || *str == '\t' || *str == '\r' || *str == '\n') {
		str++;
	}

	end = str + strlen(str);
	while (end > str &&
	       (end[-1] == ' ' || end[-1] == '\t' ||
		end[-1] == '\r' || end[-1] == '\n')) {
		end--;
	}
	*end = '\0';

	return str;
}

static int cfg_make_dir(void)
{
	if (mkdir(APP_CONFIG_DIR, 0755) != 0 &&
	    access(APP_CONFIG_DIR, F_OK) != 0) {
		printf("create config dir (%s) fail\n", APP_CONFIG_DIR);
		return -1;
	}

	return 0;
}

static void cfg_initialize_from_runtime(void)
{
	unsigned int point;

	if (g_app_config.initialized) {
		return;
	}

	memset(&g_app_config, 0, sizeof(g_app_config));
	g_app_config.boot_load = 1;
	g_app_config.fps = liveview_get_fps();
	g_app_config.output_mode = liveview_get_output_mode();
	g_app_config.startup_skip_frames =
		liveview_get_startup_skip_frames();
	g_app_config.runtime_skip_frames =
		liveview_get_runtime_skip_frames();
	g_app_config.nuc_correction = nuc_is_enabled();
	g_app_config.nuc_sensor_temp_interval_sec =
		nuc_get_sensor_temp_interval_sec();
	for (point = 0; point < APP_CONFIG_SENSOR_TEMP_POINT_COUNT; point++) {
		g_app_config.nuc_sensor_temp_points[point] =
			nuc_get_sensor_temp_point(point);
	}
	g_app_config.bpr_correction = bpr_is_enabled();
	g_app_config.bpr_threshold = bpr_get_threshold();
	g_app_config.runtime_bpr_enable = bpr_is_runtime_enabled();
	g_app_config.runtime_bpr_threshold =
		bpr_get_runtime_threshold();
	g_app_config.ffc_correction = ffc_is_enabled();
	g_app_config.shutter_interval_sec =
		ffc_get_shutter_interval_sec();
	g_app_config.shutter_close_wait_ms =
		liveview_get_shutter_close_wait_ms();
	g_app_config.tdc_correction = tdc_is_enabled();
	g_app_config.tdc_interval_sec = tdc_get_interval_sec();
	g_app_config.agc_histogram_p1_p99_enable =
		agc_histogram_p1_p99_is_enabled();
	g_app_config.agc_min_max_tone_curve_enable =
		agc_min_max_tone_curve_is_enabled();
	g_app_config.agc_fieldscale_enable = agc_fieldscale_is_enabled();
	g_app_config.videoproc_af_enable = videoproc_isp_is_af_enabled();
	g_app_config.videoproc_wdr_enable = videoproc_isp_is_wdr_enabled();
	g_app_config.videoproc_defog_enable = videoproc_isp_is_defog_enabled();
	g_app_config.videoproc_3dnr_enable = videoproc_isp_is_3dnr_enabled();
	g_app_config.initialized = 1;
}

static int cfg_parse_long(const char *value, long *parsed)
{
	char *end;
	long result;

	errno = 0;
	result = strtol(value, &end, 0);
	if (errno != 0 || end == value) {
		return -1;
	}
	while (*end == ' ' || *end == '\t') {
		end++;
	}
	if (*end != '\0') {
		return -1;
	}

	*parsed = result;
	return 0;
}

static int cfg_parse_float(const char *value, float *parsed)
{
	char *end;
	float result;

	errno = 0;
	result = strtof(value, &end);
	if (errno != 0 || end == value) {
		return -1;
	}
	while (*end == ' ' || *end == '\t') {
		end++;
	}
	if (*end != '\0') {
		return -1;
	}

	*parsed = result;
	return 0;
}

static int cfg_store_pair(APP_CONFIG_VALUES *config,
			  const char *key, const char *value)
{
	long int_value;
	float float_value;

	if (strcmp(key, "cfg_boot_load") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->boot_load = int_value ? 1 : 0;
	} else if (strcmp(key, "fps") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0) return -1;
		config->fps = (unsigned int)int_value;
	} else if (strcmp(key, "output_mode") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    (int_value != 0 && int_value != 1)) return -1;
		config->output_mode = (unsigned int)int_value;
	} else if (strcmp(key, "startup_skip_frames") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value < 0) return -1;
		config->startup_skip_frames = (unsigned int)int_value;
	} else if (strcmp(key, "runtime_skip_frames") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value < 0) return -1;
		config->runtime_skip_frames = (unsigned int)int_value;
	} else if (strcmp(key, "nuc_correction") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->nuc_correction = int_value ? 1 : 0;
	} else if (strcmp(key, "nuc_sensor_temp_interval_sec") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0) return -1;
		config->nuc_sensor_temp_interval_sec = (unsigned int)int_value;
	} else if (strcmp(key, "nuc_sensor_temp_point1") == 0) {
		if (cfg_parse_float(value, &float_value) != 0) return -1;
		config->nuc_sensor_temp_points[0] = float_value;
	} else if (strcmp(key, "nuc_sensor_temp_point2") == 0) {
		if (cfg_parse_float(value, &float_value) != 0) return -1;
		config->nuc_sensor_temp_points[1] = float_value;
	} else if (strcmp(key, "nuc_sensor_temp_point3") == 0) {
		if (cfg_parse_float(value, &float_value) != 0) return -1;
		config->nuc_sensor_temp_points[2] = float_value;
	} else if (strcmp(key, "nuc_sensor_temp_point4") == 0) {
		if (cfg_parse_float(value, &float_value) != 0) return -1;
		config->nuc_sensor_temp_points[3] = float_value;
	} else if (strcmp(key, "nuc_sensor_temp_point5") == 0) {
		if (cfg_parse_float(value, &float_value) != 0) return -1;
		config->nuc_sensor_temp_points[4] = float_value;
	} else if (strcmp(key, "bpr_correction") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->bpr_correction = int_value ? 1 : 0;
	} else if (strcmp(key, "bpr_threshold") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value < 0) return -1;
		config->bpr_threshold = (unsigned int)int_value;
	} else if (strcmp(key, "runtime_bpr_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->runtime_bpr_enable = int_value ? 1 : 0;
	} else if (strcmp(key, "runtime_bpr_threshold") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    int_value < 0 || int_value > 100) return -1;
		config->runtime_bpr_threshold = (unsigned int)int_value;
	} else if (strcmp(key, "ffc_correction") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->ffc_correction = int_value ? 1 : 0;
	} else if (strcmp(key, "shutter_interval_sec") == 0 ||
		   strcmp(key, "ffc_interval_sec") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0) return -1;
		config->shutter_interval_sec = (unsigned int)int_value;
	} else if (strcmp(key, "shutter_close_wait_ms") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0) return -1;
		config->shutter_close_wait_ms = (unsigned int)int_value;
	} else if (strcmp(key, "tdc_correction") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->tdc_correction = int_value ? 1 : 0;
	} else if (strcmp(key, "tdc_interval_sec") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0) return -1;
		config->tdc_interval_sec = (unsigned int)int_value;
	} else if (strcmp(key, "agc_histogram_p1_p99_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->agc_histogram_p1_p99_enable = int_value ? 1 : 0;
		if (config->agc_histogram_p1_p99_enable) {
			config->agc_fieldscale_enable = 0;
			if (config->agc_min_max_tone_curve_enable) {
				printf("AGC mutual exclusion: disable Min/Max Tone Curve AGC (ISP)\n");
			}
			config->agc_min_max_tone_curve_enable = 0;
		}
	} else if (strcmp(key, "agc_min_max_tone_curve_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->agc_min_max_tone_curve_enable = int_value ? 1 : 0;
		if (config->agc_min_max_tone_curve_enable) {
			config->agc_fieldscale_enable = 0;
			if (config->agc_histogram_p1_p99_enable) {
				printf("AGC mutual exclusion: disable Histogram P1/P99 AGC (CPU)\n");
			}
			config->agc_histogram_p1_p99_enable = 0;
		}
	} else if (strcmp(key, "agc_fieldscale_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->agc_fieldscale_enable = int_value ? 1 : 0;
		if (config->agc_fieldscale_enable) {
			if (config->agc_histogram_p1_p99_enable ||
			    config->agc_min_max_tone_curve_enable) {
				printf("AGC mutual exclusion: disable other AGC modes\n");
			}
			config->agc_histogram_p1_p99_enable = 0;
			config->agc_min_max_tone_curve_enable = 0;
		}
	} else if (strcmp(key, "videoproc_af_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->videoproc_af_enable = int_value ? 1 : 0;
	} else if (strcmp(key, "videoproc_wdr_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->videoproc_wdr_enable = int_value ? 1 : 0;
	} else if (strcmp(key, "videoproc_defog_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->videoproc_defog_enable = int_value ? 1 : 0;
	} else if (strcmp(key, "videoproc_3dnr_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->videoproc_3dnr_enable = int_value ? 1 : 0;
	} else {
		return 1;
	}

	return 0;
}

static int cfg_apply_runtime(const APP_CONFIG_VALUES *config)
{
	unsigned int point;
	int apply_result = 0;

	if (liveview_set_fps(config->fps) != 0) {
		apply_result = -1;
	}
	if (liveview_set_output_mode(config->output_mode) != 0) {
		apply_result = -1;
	}
	liveview_set_startup_skip_frames(config->startup_skip_frames);
	liveview_set_runtime_skip_frames(config->runtime_skip_frames);

	nuc_set_sensor_temp_interval_sec(
		config->nuc_sensor_temp_interval_sec);
	for (point = 0; point < APP_CONFIG_SENSOR_TEMP_POINT_COUNT; point++) {
		nuc_set_sensor_temp_point(
			point, config->nuc_sensor_temp_points[point]);
	}
	nuc_set_enabled(config->nuc_correction);

	bpr_set_threshold(config->bpr_threshold);
	if (bpr_set_enabled(config->bpr_correction) < 0) {
		apply_result = -1;
	}
	bpr_set_runtime_threshold(config->runtime_bpr_threshold);
	bpr_set_runtime_enabled(config->runtime_bpr_enable);

	ffc_set_shutter_interval_sec(config->shutter_interval_sec);
	liveview_set_shutter_close_wait_ms(config->shutter_close_wait_ms);
	ffc_set_enabled(config->ffc_correction);
	tdc_set_interval_sec(config->tdc_interval_sec);
	tdc_set_enabled(config->tdc_correction);
	if (config->agc_histogram_p1_p99_enable +
	    config->agc_min_max_tone_curve_enable +
	    config->agc_fieldscale_enable > 1) {
		printf("invalid AGC CFG: multiple AGCs are enabled; keep all off\n");
		agc_mode_set(AGC_MODE_OFF);
		apply_result = -1;
	} else if (config->agc_histogram_p1_p99_enable) {
		agc_mode_set(AGC_MODE_HISTOGRAM_P1_P99_CPU);
	} else if (config->agc_min_max_tone_curve_enable) {
		agc_mode_set(AGC_MODE_MIN_MAX_TONE_CURVE_ISP);
	} else if (config->agc_fieldscale_enable) {
		agc_mode_set(AGC_MODE_FIELDSCALE_CPU);
	} else {
		agc_mode_set(AGC_MODE_OFF);
	}
	videoproc_isp_set_runtime(config->videoproc_af_enable,
				  config->videoproc_wdr_enable,
				  config->videoproc_defog_enable,
				  config->videoproc_3dnr_enable);
	return apply_result;
}

int app_config_load(void)
{
	FILE *fp;
	char line[128];
	APP_CONFIG_VALUES loaded_config;

	cfg_initialize_from_runtime();
	loaded_config = g_app_config;

	fp = fopen(APP_CONFIG_PATH, "r");
	if (fp == 0) {
		printf("config not found, create default: %s\n", APP_CONFIG_PATH);
		if (app_config_save() != 0) {
			return -1;
		}
		return cfg_apply_runtime(&g_app_config);
	}

	while (fgets(line, sizeof(line), fp) != 0) {
		char *key;
		char *value;
		char *equal;
		int store_ret;

		key = cfg_trim(line);
		if (key[0] == '\0' || key[0] == '#') {
			continue;
		}

		equal = strchr(key, '=');
		if (equal == 0) {
			continue;
		}

		*equal = '\0';
		value = cfg_trim(equal + 1);
		key = cfg_trim(key);
		store_ret = cfg_store_pair(&loaded_config, key, value);
		if (store_ret < 0) {
			printf("invalid cfg %s=%s, keep pending value\n",
			       key, value);
		}
	}
	fclose(fp);

	loaded_config.initialized = 1;
	g_app_config = loaded_config;
	if (!g_app_config.boot_load) {
		printf("config boot load disabled, keep current runtime settings\n");
		return 0;
	}

	if (cfg_apply_runtime(&g_app_config) != 0) {
		return -1;
	}
	printf("config loaded and applied: %s\n", APP_CONFIG_PATH);
	return 0;
}

void app_config_print_restart_required(void)
{
	printf("CFG setting saved. Current runtime is unchanged.\n");
	if (g_app_config.boot_load) {
		printf("Use Liveview Restart or reboot to apply the new setting.\n");
	} else {
		printf("CFG boot load is off. Enable it, then use Liveview Restart or reboot.\n");
	}
}

int app_config_get_int(const char *key, int *value)
{
	if (key == 0 || value == 0) {
		return -1;
	}

	cfg_initialize_from_runtime();
	if (strcmp(key, "cfg_boot_load") == 0) {
		*value = g_app_config.boot_load;
	} else if (strcmp(key, "fps") == 0) {
		*value = (int)g_app_config.fps;
	} else if (strcmp(key, "output_mode") == 0) {
		*value = (int)g_app_config.output_mode;
	} else if (strcmp(key, "startup_skip_frames") == 0) {
		*value = (int)g_app_config.startup_skip_frames;
	} else if (strcmp(key, "runtime_skip_frames") == 0) {
		*value = (int)g_app_config.runtime_skip_frames;
	} else if (strcmp(key, "nuc_correction") == 0) {
		*value = g_app_config.nuc_correction;
	} else if (strcmp(key, "nuc_sensor_temp_interval_sec") == 0) {
		*value = (int)g_app_config.nuc_sensor_temp_interval_sec;
	} else if (strcmp(key, "bpr_correction") == 0) {
		*value = g_app_config.bpr_correction;
	} else if (strcmp(key, "bpr_threshold") == 0) {
		*value = (int)g_app_config.bpr_threshold;
	} else if (strcmp(key, "runtime_bpr_enable") == 0) {
		*value = g_app_config.runtime_bpr_enable;
	} else if (strcmp(key, "runtime_bpr_threshold") == 0) {
		*value = (int)g_app_config.runtime_bpr_threshold;
	} else if (strcmp(key, "ffc_correction") == 0) {
		*value = g_app_config.ffc_correction;
	} else if (strcmp(key, "shutter_interval_sec") == 0) {
		*value = (int)g_app_config.shutter_interval_sec;
	} else if (strcmp(key, "shutter_close_wait_ms") == 0) {
		*value = (int)g_app_config.shutter_close_wait_ms;
	} else if (strcmp(key, "tdc_correction") == 0) {
		*value = g_app_config.tdc_correction;
	} else if (strcmp(key, "tdc_interval_sec") == 0) {
		*value = (int)g_app_config.tdc_interval_sec;
	} else if (strcmp(key, "agc_histogram_p1_p99_enable") == 0) {
		*value = g_app_config.agc_histogram_p1_p99_enable;
	} else if (strcmp(key, "agc_min_max_tone_curve_enable") == 0) {
		*value = g_app_config.agc_min_max_tone_curve_enable;
	} else if (strcmp(key, "agc_fieldscale_enable") == 0) {
		*value = g_app_config.agc_fieldscale_enable;
	} else if (strcmp(key, "videoproc_af_enable") == 0) {
		*value = g_app_config.videoproc_af_enable;
	} else if (strcmp(key, "videoproc_wdr_enable") == 0) {
		*value = g_app_config.videoproc_wdr_enable;
	} else if (strcmp(key, "videoproc_defog_enable") == 0) {
		*value = g_app_config.videoproc_defog_enable;
	} else if (strcmp(key, "videoproc_3dnr_enable") == 0) {
		*value = g_app_config.videoproc_3dnr_enable;
	} else {
		return -1;
	}
	return 0;
}

int app_config_update_int(const char *key, int value)
{
	char value_text[32];
	APP_CONFIG_VALUES old_config;
	int ret;

	if (key == 0) {
		return -1;
	}

	cfg_initialize_from_runtime();
	old_config = g_app_config;
	snprintf(value_text, sizeof(value_text), "%d", value);
	ret = cfg_store_pair(&g_app_config, key, value_text);
	if (ret != 0) {
		printf("invalid CFG setting %s=%s\n", key, value_text);
		g_app_config = old_config;
		return -1;
	}
	if (app_config_save() != 0) {
		g_app_config = old_config;
		return -1;
	}

	app_config_print_restart_required();
	return 0;
}

int app_config_update_videoproc_isp(int af_enable, int wdr_enable,
				    int defog_enable, int three_dnr_enable)
{
	APP_CONFIG_VALUES old_config;

	cfg_initialize_from_runtime();
	old_config = g_app_config;
	g_app_config.videoproc_af_enable = af_enable ? 1 : 0;
	g_app_config.videoproc_wdr_enable = wdr_enable ? 1 : 0;
	g_app_config.videoproc_defog_enable = defog_enable ? 1 : 0;
	g_app_config.videoproc_3dnr_enable = three_dnr_enable ? 1 : 0;
	if (app_config_save() != 0) {
		g_app_config = old_config;
		return -1;
	}

	app_config_print_restart_required();
	return 0;
}

int app_config_update_sensor_temp_points(const float *points,
					 unsigned int point_count)
{
	APP_CONFIG_VALUES old_config;
	unsigned int point;

	if (points == 0 ||
	    point_count != APP_CONFIG_SENSOR_TEMP_POINT_COUNT) {
		return -1;
	}

	cfg_initialize_from_runtime();
	old_config = g_app_config;
	for (point = 0; point < point_count; point++) {
		g_app_config.nuc_sensor_temp_points[point] = points[point];
	}
	if (app_config_save() != 0) {
		g_app_config = old_config;
		return -1;
	}

	app_config_print_restart_required();
	return 0;
}

int app_config_get_boot_load(void)
{
	cfg_initialize_from_runtime();
	return g_app_config.boot_load;
}

static int app_config_show_file(void)
{
	char key[32];
	unsigned int point;

	cfg_initialize_from_runtime();

	printf("============================================================\n");
	printf("Current CFG\n");
	printf("Path : %s\n", APP_CONFIG_PATH);
	printf("============================================================\n");

	printf("\n[General]\n");
	printf("  1. %-31s = %d (%s)\n", "cfg_boot_load",
	       g_app_config.boot_load,
	       g_app_config.boot_load ? "on" : "off");

	printf("\n[Liveview]\n");
	printf("  1. %-31s = %u (%s)\n", "output_mode",
	       g_app_config.output_mode,
	       g_app_config.output_mode == 1U ? "RTSP" : "UVC");
	printf("  2. %-31s = %u fps\n", "fps", g_app_config.fps);
	printf("  3. %-31s = %u frames\n", "startup_skip_frames",
	       g_app_config.startup_skip_frames);
	printf("  4. %-31s = %u frames\n", "runtime_skip_frames",
	       g_app_config.runtime_skip_frames);

	printf("\n[BPR]\n");
	printf("  1. %-31s = %d (%s)\n", "bpr_correction",
	       g_app_config.bpr_correction,
	       g_app_config.bpr_correction ? "on" : "off");
	printf("  2. %-31s = %u RAW8 DN\n", "bpr_threshold",
	       g_app_config.bpr_threshold);
	printf("  3. %-31s = %d (%s)\n", "runtime_bpr_enable",
	       g_app_config.runtime_bpr_enable,
	       g_app_config.runtime_bpr_enable ? "on" : "off");
	printf("  4. %-31s = %u RAW8 DN\n", "runtime_bpr_threshold",
	       g_app_config.runtime_bpr_threshold);

	printf("\n[Shutter / FFC]\n");
	printf("  1. %-31s = %d (%s)\n", "ffc_correction",
	       g_app_config.ffc_correction,
	       g_app_config.ffc_correction ? "on" : "off");
	printf("  2. %-31s = %u sec\n", "shutter_interval_sec",
	       g_app_config.shutter_interval_sec);
	printf("  3. %-31s = %u ms\n", "shutter_close_wait_ms",
	       g_app_config.shutter_close_wait_ms);

	printf("\n[NUC]\n");
	printf("  1. %-31s = %d (%s)\n", "nuc_correction",
	       g_app_config.nuc_correction,
	       g_app_config.nuc_correction ? "on" : "off");
	printf("  2. %-31s = %u sec\n", "nuc_sensor_temp_interval_sec",
	       g_app_config.nuc_sensor_temp_interval_sec);
	for (point = 0; point < APP_CONFIG_SENSOR_TEMP_POINT_COUNT; point++) {
		snprintf(key, sizeof(key), "nuc_sensor_temp_point%u",
			 point + 1U);
		printf("  %u. %-31s = %.4f C\n", point + 3U, key,
		       g_app_config.nuc_sensor_temp_points[point]);
	}

	printf("\n[TDC]\n");
	printf("  1. %-31s = %d (%s)\n", "tdc_correction",
	       g_app_config.tdc_correction,
	       g_app_config.tdc_correction ? "on" : "off");
	printf("  2. %-31s = %u sec\n", "tdc_interval_sec",
	       g_app_config.tdc_interval_sec);

	printf("\n[AGC - mutually exclusive]\n");
	printf("  1. %-31s = %d (%s)\n",
	       "agc_histogram_p1_p99_enable",
	       g_app_config.agc_histogram_p1_p99_enable,
	       g_app_config.agc_histogram_p1_p99_enable ? "on" : "off");
	printf("  2. %-31s = %d (%s)\n",
	       "agc_min_max_tone_curve_enable",
	       g_app_config.agc_min_max_tone_curve_enable,
	       g_app_config.agc_min_max_tone_curve_enable ? "on" : "off");
	printf("  3. %-31s = %d (%s)\n",
	       "agc_fieldscale_enable",
	       g_app_config.agc_fieldscale_enable,
	       g_app_config.agc_fieldscale_enable ? "on" : "off");
	printf("     selected runtime AGC: %s\n",
	       agc_mode_name(agc_mode_get()));

	printf("\n[VideoProc / ISP]\n");
	printf("  1. %-31s = %d (%s)\n", "videoproc_af_enable",
	       g_app_config.videoproc_af_enable,
	       g_app_config.videoproc_af_enable ? "on" : "off");
	printf("  2. %-31s = %d (%s)\n", "videoproc_wdr_enable",
	       g_app_config.videoproc_wdr_enable,
	       g_app_config.videoproc_wdr_enable ? "on" : "off");
	printf("  3. %-31s = %d (%s)\n", "videoproc_defog_enable",
	       g_app_config.videoproc_defog_enable,
	       g_app_config.videoproc_defog_enable ? "on" : "off");
	printf("  4. %-31s = %d (%s)\n", "videoproc_3dnr_enable",
	       g_app_config.videoproc_3dnr_enable,
	       g_app_config.videoproc_3dnr_enable ? "on" : "off");

	printf("\n");
	return 0;
}

static int app_config_prompt_int(const char *label, const char *key,
				 int current, int min_value, int max_value,
				 const char *unit)
{
	int value;

	if (max_value == INT_MAX) {
		printf("%s (minimum %d%s, pending CFG %d%s)\n",
		       label, min_value, unit, current, unit);
	} else {
		printf("%s (%d-%d%s, pending CFG %d%s)\n",
		       label, min_value, max_value, unit, current, unit);
	}

	value = app_config_read_option();
	if (value == -1 || value == MENU_OPTION_REFRESH) {
		return 0;
	}
	if (value < min_value || value > max_value) {
		printf("Invalid %s, keep pending CFG %d%s\n",
		       label, current, unit);
		return -1;
	}

	return app_config_update_int(key, value);
}

static int app_config_prompt_bool(const char *label, const char *key,
				  int current)
{
	int value;

	printf("%s (0: off, 1: on, pending CFG: %s)\n",
	       label, current ? "on" : "off");
	value = app_config_read_option();
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

static const char *app_config_output_mode_name(unsigned int mode)
{
	return mode == 1U ? "RTSP" : "UVC";
}

static int app_config_prompt_output_mode(void)
{
	int value;

	printf("Output mode (0: UVC, 1: RTSP, pending CFG: %s)\n",
	       app_config_output_mode_name(g_app_config.output_mode));
	value = app_config_read_option();
	if (value == -1 || value == MENU_OPTION_REFRESH) {
		return 0;
	}
	if (value != 0 && value != 1) {
		printf("Invalid output mode, enter 0 or 1; keep pending CFG %s\n",
		       app_config_output_mode_name(g_app_config.output_mode));
		return -1;
	}

	return app_config_update_int("output_mode", value);
}

static int app_config_liveview_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Liveview setting\n");
		printf(" 1. FPS\n");
		printf(" 2. Startup skip frames\n");
		printf(" 3. Runtime skip frames\n");
		printf(" 4. Output mode (UVC / RTSP)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: output=%s, fps=%u, startup skip=%u, runtime skip=%u\n",
		       app_config_output_mode_name(liveview_get_output_mode()),
		       liveview_get_fps(), liveview_get_startup_skip_frames(),
		       liveview_get_runtime_skip_frames());
		printf("pending CFG: output=%s, fps=%u, startup skip=%u, runtime skip=%u\n",
		       app_config_output_mode_name(g_app_config.output_mode),
		       g_app_config.fps, g_app_config.startup_skip_frames,
		       g_app_config.runtime_skip_frames);

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_int("FPS", "fps",
				(int)g_app_config.fps, 1, INT_MAX, "");
			break;
		case 2:
			ret = app_config_prompt_int("Startup skip frames",
				"startup_skip_frames",
				(int)g_app_config.startup_skip_frames,
				0, INT_MAX, "");
			break;
		case 3:
			ret = app_config_prompt_int("Runtime skip frames",
				"runtime_skip_frames",
				(int)g_app_config.runtime_skip_frames,
				0, INT_MAX, "");
			break;
		case 4:
			ret = app_config_prompt_output_mode();
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

static int app_config_bpr_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("BPR setting\n");
		printf(" 1. BPR on/off\n");
		printf(" 2. Bright/Dark BPR threshold (RAW8 DN)\n");
		printf(" 3. Runtime BPR on/off\n");
		printf(" 4. Runtime BPR threshold (RAW8 DN)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: BPR=%s, threshold=%u RAW8 DN, "
		       "Runtime BPR=%s, threshold=%u RAW8 DN\n",
		       bpr_is_enabled() ? "on" : "off", bpr_get_threshold(),
		       bpr_is_runtime_enabled() ? "on" : "off",
		       bpr_get_runtime_threshold());
		printf("pending CFG: BPR=%s, threshold=%u RAW8 DN, "
		       "Runtime BPR=%s, threshold=%u RAW8 DN\n",
		       g_app_config.bpr_correction ? "on" : "off",
		       g_app_config.bpr_threshold,
		       g_app_config.runtime_bpr_enable ? "on" : "off",
		       g_app_config.runtime_bpr_threshold);

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool("BPR", "bpr_correction",
				g_app_config.bpr_correction);
			break;
		case 2:
			ret = app_config_prompt_int("Bright/Dark BPR threshold",
				"bpr_threshold",
				(int)g_app_config.bpr_threshold,
				0, 100, " RAW8 DN");
			break;
		case 3:
			ret = app_config_prompt_bool("Runtime BPR",
				"runtime_bpr_enable",
				g_app_config.runtime_bpr_enable);
			break;
		case 4:
			ret = app_config_prompt_int("Runtime BPR threshold",
				"runtime_bpr_threshold",
				(int)g_app_config.runtime_bpr_threshold,
				0, 100, " RAW8 DN");
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

static int app_config_shutter_ffc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("Shutter / FFC setting\n");
		printf(" 1. FFC on/off\n");
		printf(" 2. Shared shutter interval\n");
		printf(" 3. Shutter close wait time\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: FFC=%s, shutter interval=%u sec, close wait=%u ms\n",
		       ffc_is_enabled() ? "on" : "off",
		       ffc_get_shutter_interval_sec(),
		       liveview_get_shutter_close_wait_ms());
		printf("pending CFG: FFC=%s, shutter interval=%u sec, close wait=%u ms\n",
		       g_app_config.ffc_correction ? "on" : "off",
		       g_app_config.shutter_interval_sec,
		       g_app_config.shutter_close_wait_ms);
		printf("FFC and Runtime BPR share this shutter interval.\n");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool("FFC", "ffc_correction",
				g_app_config.ffc_correction);
			break;
		case 2:
			ret = app_config_prompt_int("Shared shutter interval",
				"shutter_interval_sec",
				(int)g_app_config.shutter_interval_sec,
				1, INT_MAX, " sec");
			break;
		case 3:
			ret = app_config_prompt_int("Shutter close wait time",
				"shutter_close_wait_ms",
				(int)g_app_config.shutter_close_wait_ms,
				1, INT_MAX, " ms");
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

static int app_config_nuc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("NUC setting\n");
		printf(" 1. NUC on/off\n");
		printf(" 2. Sensor Temperature table interval\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: NUC=%s, interval=%u sec\n",
		       nuc_is_enabled() ? "on" : "off",
		       nuc_get_sensor_temp_interval_sec());
		printf("pending CFG: NUC=%s, interval=%u sec\n",
		       g_app_config.nuc_correction ? "on" : "off",
		       g_app_config.nuc_sensor_temp_interval_sec);

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool("NUC", "nuc_correction",
				g_app_config.nuc_correction);
			break;
		case 2:
			ret = app_config_prompt_int(
				"Sensor Temperature table interval",
				"nuc_sensor_temp_interval_sec",
				(int)g_app_config.nuc_sensor_temp_interval_sec,
				1, INT_MAX, " sec");
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

static int app_config_tdc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("TDC setting\n");
		printf(" 1. TDC on/off\n");
		printf(" 2. TDC interval\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: TDC=%s, interval=%u sec\n",
		       tdc_is_enabled() ? "on" : "off",
		       tdc_get_interval_sec());
		printf("pending CFG: TDC=%s, interval=%u sec\n",
		       g_app_config.tdc_correction ? "on" : "off",
		       g_app_config.tdc_interval_sec);

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool("TDC", "tdc_correction",
				g_app_config.tdc_correction);
			break;
		case 2:
			ret = app_config_prompt_int("TDC interval",
				"tdc_interval_sec",
				(int)g_app_config.tdc_interval_sec,
				1, INT_MAX, " sec");
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

static int app_config_agc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("AGC setting (mutually exclusive)\n");
		printf(" 1. Histogram P1/P99 AGC (CPU) on/off\n");
		printf(" 2. Min/Max Tone Curve AGC (ISP) on/off\n");
		printf(" 3. Fieldscale Local Adaptive AGC (CPU) on/off\n");
		printf(" 4. Disable all AGC\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("selected runtime AGC: %s\n",
		       agc_mode_name(agc_mode_get()));
		printf("pending CFG: Histogram P1/P99=%s, "
		       "Min/Max Tone Curve=%s, Fieldscale=%s\n",
		       g_app_config.agc_histogram_p1_p99_enable ? "on" : "off",
		       g_app_config.agc_min_max_tone_curve_enable ? "on" : "off",
		       g_app_config.agc_fieldscale_enable ? "on" : "off");
		printf("enabling one AGC automatically disables the other AGCs\n");
		printf("pipeline: after RAW snapshot, before VideoProc\n");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool(
				"Histogram P1/P99 AGC (CPU)",
				"agc_histogram_p1_p99_enable",
				g_app_config.agc_histogram_p1_p99_enable);
			break;
		case 2:
			ret = app_config_prompt_bool(
				"Min/Max Tone Curve AGC (ISP)",
				"agc_min_max_tone_curve_enable",
				g_app_config.agc_min_max_tone_curve_enable);
			break;
		case 3:
			ret = app_config_prompt_bool(
				"Fieldscale Local Adaptive AGC (CPU)",
				"agc_fieldscale_enable",
				g_app_config.agc_fieldscale_enable);
			break;
		case 4:
			g_app_config.agc_histogram_p1_p99_enable = 0;
			g_app_config.agc_min_max_tone_curve_enable = 0;
			g_app_config.agc_fieldscale_enable = 0;
			ret = app_config_save();
			if (ret == 0) {
				printf("All pending CFG AGC modes disabled\n");
				app_config_print_restart_required();
			}
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

int app_config_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		cfg_initialize_from_runtime();
		printf("----------------------------------------\n");
		printf("CFG Setting\n");
		printf(" 1. Liveview setting\n");
		printf(" 2. BPR setting\n");
		printf(" 3. FFC setting / Shutter\n");
		printf(" 4. NUC setting\n");
		printf(" 5. TDC setting\n");
		printf(" 6. AGC setting (mutually exclusive)\n");
		printf(" 7. VideoProc / ISP setting\n");
		printf(" 8. Show current tiffany_thermal.cfg\n");
		printf(" 9. Generate tiffany_thermal.cfg\n");
		printf("10. Use CFG on next boot/restart on/off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("pending CFG: boot load %s\n",
		       app_config_get_boot_load() ? "on" : "off");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_liveview_menu_run();
			break;
		case 2:
			ret = app_config_bpr_menu_run();
			break;
		case 3:
			ret = app_config_shutter_ffc_menu_run();
			break;
		case 4:
			ret = app_config_nuc_menu_run();
			break;
		case 5:
			ret = app_config_tdc_menu_run();
			break;
		case 6:
			ret = app_config_agc_menu_run();
			break;
		case 7:
			ret = videoproc_isp_menu_run();
			break;
		case 8:
			ret = app_config_show_file();
			break;
		case 9:
			ret = app_config_save();
			break;
		case 10:
			ret = app_config_prompt_bool(
				"Use CFG on next boot/restart",
				"cfg_boot_load",
				app_config_get_boot_load());
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

int app_config_save(void)
{
	FILE *fp = NULL;
	unsigned int point;
#if defined(__LINUX)
	int dir_fd;
#endif

	cfg_initialize_from_runtime();
	if (cfg_make_dir() != 0) {
		return -1;
	}

	/*
	 * Keep the current config intact until a complete replacement has been
	 * written and synchronized on the same filesystem.
	 */
	fp = fopen(APP_CONFIG_TEMP_PATH, "w");
	if (fp == 0) {
		printf("open config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
		return -1;
	}

	fprintf(fp, "# Tiffany thermal user settings\n");
	fprintf(fp, "cfg_boot_load=%d\n", g_app_config.boot_load);
	fprintf(fp, "fps=%u\n", g_app_config.fps);
	fprintf(fp, "output_mode=%u\n", g_app_config.output_mode);
	fprintf(fp, "startup_skip_frames=%u\n",
		g_app_config.startup_skip_frames);
	fprintf(fp, "runtime_skip_frames=%u\n",
		g_app_config.runtime_skip_frames);
	fprintf(fp, "nuc_correction=%d\n", g_app_config.nuc_correction);
	fprintf(fp, "nuc_sensor_temp_interval_sec=%u\n",
		g_app_config.nuc_sensor_temp_interval_sec);
	for (point = 0; point < APP_CONFIG_SENSOR_TEMP_POINT_COUNT; point++) {
		fprintf(fp, "nuc_sensor_temp_point%u=%.4f\n",
			point + 1,
			g_app_config.nuc_sensor_temp_points[point]);
	}
	fprintf(fp, "bpr_correction=%d\n", g_app_config.bpr_correction);
	fprintf(fp, "bpr_threshold=%u\n", g_app_config.bpr_threshold);
	fprintf(fp, "runtime_bpr_enable=%d\n",
		g_app_config.runtime_bpr_enable);
	fprintf(fp, "runtime_bpr_threshold=%u\n",
		g_app_config.runtime_bpr_threshold);
	fprintf(fp, "ffc_correction=%d\n", g_app_config.ffc_correction);
	fprintf(fp, "shutter_interval_sec=%u\n",
		g_app_config.shutter_interval_sec);
	fprintf(fp, "shutter_close_wait_ms=%u\n",
		g_app_config.shutter_close_wait_ms);
	fprintf(fp, "tdc_correction=%d\n", g_app_config.tdc_correction);
	fprintf(fp, "tdc_interval_sec=%u\n",
		g_app_config.tdc_interval_sec);
	fprintf(fp, "agc_histogram_p1_p99_enable=%d\n",
		g_app_config.agc_histogram_p1_p99_enable);
	fprintf(fp, "agc_min_max_tone_curve_enable=%d\n",
		g_app_config.agc_min_max_tone_curve_enable);
	fprintf(fp, "agc_fieldscale_enable=%d\n",
		g_app_config.agc_fieldscale_enable);
	fprintf(fp, "videoproc_af_enable=%d\n",
		g_app_config.videoproc_af_enable);
	fprintf(fp, "videoproc_wdr_enable=%d\n",
		g_app_config.videoproc_wdr_enable);
	fprintf(fp, "videoproc_defog_enable=%d\n",
		g_app_config.videoproc_defog_enable);
	fprintf(fp, "videoproc_3dnr_enable=%d\n",
		g_app_config.videoproc_3dnr_enable);
	if (ferror(fp)) {
		printf("write config temp (%s) fail\n", APP_CONFIG_TEMP_PATH);
		goto save_fail;
	}
	if (fflush(fp) != 0) {
		printf("flush config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
		goto save_fail;
	}

#if defined(__LINUX)
	if (fsync(fileno(fp)) != 0) {
		printf("sync config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
		goto save_fail;
	}
#endif

	if (fclose(fp) != 0) {
		fp = NULL;
		printf("close config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
		goto save_fail;
	}
	/* fclose() has consumed the stream even when it reports an error. */
	fp = NULL;

	if (rename(APP_CONFIG_TEMP_PATH, APP_CONFIG_PATH) != 0) {
		printf("replace config (%s) fail: %s\n",
		       APP_CONFIG_PATH, strerror(errno));
		goto save_fail;
	}

#if defined(__LINUX)
	/* Best effort: persist the directory entry created by rename(). */
	dir_fd = open(APP_CONFIG_DIR, O_RDONLY);
	if (dir_fd < 0) {
		printf("open config directory (%s) for sync fail: %s\n",
		       APP_CONFIG_DIR, strerror(errno));
	} else {
		if (fsync(dir_fd) != 0) {
			printf("sync config directory (%s) fail: %s\n",
			       APP_CONFIG_DIR, strerror(errno));
		}
		if (close(dir_fd) != 0) {
			printf("close config directory (%s) fail: %s\n",
			       APP_CONFIG_DIR, strerror(errno));
		}
	}
#endif

	printf("config saved: %s\n", APP_CONFIG_PATH);
	return 0;

save_fail:
	if (fp != NULL && fclose(fp) != 0) {
		printf("close failed config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
	}
	if (remove(APP_CONFIG_TEMP_PATH) != 0 && errno != ENOENT) {
		printf("remove config temp (%s) fail: %s\n",
		       APP_CONFIG_TEMP_PATH, strerror(errno));
	}
	return -1;
}
