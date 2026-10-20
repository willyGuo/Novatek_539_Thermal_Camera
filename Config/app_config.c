#include "app_config.h"
#include "../Enhancement/CLAHE/clahe.h"
#include "../Enhancement/DDE/dde.h"

#include <errno.h>
#include <stddef.h>
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
#include "../ISP/videoproc/videoproc_isp.h"
#include "../ISP/menu.h"
#include "../Pipeline/liveview.h"
#include "../ISP/Pseudo/pseudo.h"

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
	int liveview_mirror;
	int pseudo_color_mode;
	unsigned int startup_skip_frames;
	unsigned int runtime_skip_frames;
	int nuc_correction;
	unsigned int nuc_sensor_temp_interval_sec;
	float nuc_sensor_temp_points[APP_CONFIG_SENSOR_TEMP_POINT_COUNT];
	int bpr_correction;
	unsigned int bpr_threshold;
	int ffc_correction;
	int ffc_startup_enable;
	unsigned int shutter_interval_sec;
	unsigned int shutter_close_wait_ms;
	int tdc_correction;
	unsigned int tdc_interval_sec;
	int agc_histogram_p1_p99_enable;
	AGC_HISTOGRAM_P1_P99_PARAMS histogram_p1_p99;
	int agc_min_max_tone_curve_enable;
	int agc_fieldscale_enable;
	AGC_FIELDSCALE_PARAMS fieldscale;
	CLAHE_PARAMS clahe;
    DDE_PARAMS dde;
	int videoproc_af_enable;
	int videoproc_wdr_enable;
	int videoproc_defog_enable;
	int videoproc_3dnr_enable;
} APP_CONFIG_VALUES;

static APP_CONFIG_VALUES g_app_config;

typedef struct {
    const char *key;
    const char *label;
    size_t offset;
    int minimum;
    int maximum;
} FIELDSCALE_CFG_ITEM;

static const FIELDSCALE_CFG_ITEM fieldscale_items[] = {
    {"agc_fieldscale_grid_size", "Grid size (2/4/8/16/32 per axis)", offsetof(AGC_FIELDSCALE_PARAMS, grid_size), 2, 32},
    {"agc_fieldscale_les_distance", "LES Distance (grid cells)", offsetof(AGC_FIELDSCALE_PARAMS, les_distance), 1, 31},
    {"agc_fieldscale_les_threshold", "LES Threshold TLES (RAW14 DN)", offsetof(AGC_FIELDSCALE_PARAMS, les_threshold), 0, 16383},
    {"agc_fieldscale_message_distance", "Message Passing Distance (grid cells)", offsetof(AGC_FIELDSCALE_PARAMS, message_distance), 1, 31},
    {"agc_fieldscale_message_iterations", "Message Passing iterations N (0: bypass)", offsetof(AGC_FIELDSCALE_PARAMS, message_iterations), 0, 32},
    {"agc_fieldscale_temporal_previous_percent", "Temporal Smoothing (previous %, 0: off)", offsetof(AGC_FIELDSCALE_PARAMS, temporal_previous_percent), 0, 99}
};
#define FIELDSCALE_CFG_COUNT (sizeof(fieldscale_items) / sizeof(fieldscale_items[0]))

static int cfg_fieldscale_index(const char *key)
{
    unsigned int i;
    for (i = 0; i < FIELDSCALE_CFG_COUNT; i++) {
        if (strcmp(key, fieldscale_items[i].key) == 0) return (int)i;
    }
    return -1;
}

static uint32_t *cfg_fieldscale_value(AGC_FIELDSCALE_PARAMS *p, unsigned int i)
{
    return (uint32_t *)((unsigned char *)p + fieldscale_items[i].offset);
}

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
	clahe_get_params(&g_app_config.clahe);
    dde_get_params(&g_app_config.dde);
	g_app_config.fps = liveview_get_fps();
	g_app_config.output_mode = liveview_get_output_mode();
	g_app_config.liveview_mirror = liveview_get_mirror();
	g_app_config.pseudo_color_mode = pseudo_get_mode();
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
	g_app_config.ffc_correction = ffc_is_enabled();
	g_app_config.ffc_startup_enable = ffc_is_startup_enabled();
	g_app_config.shutter_interval_sec =
		ffc_get_shutter_interval_sec();
	g_app_config.shutter_close_wait_ms =
		liveview_get_shutter_close_wait_ms();
	g_app_config.tdc_correction = tdc_is_enabled();
	g_app_config.tdc_interval_sec = tdc_get_interval_sec();
	g_app_config.agc_histogram_p1_p99_enable =
		agc_histogram_p1_p99_is_enabled();
	agc_histogram_p1_p99_get_params(&g_app_config.histogram_p1_p99);
	g_app_config.agc_min_max_tone_curve_enable =
		agc_min_max_tone_curve_is_enabled();
	g_app_config.agc_fieldscale_enable = agc_fieldscale_is_enabled();
	agc_fieldscale_get_params(&g_app_config.fieldscale);
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
    int fieldscale_index = cfg_fieldscale_index(key);

    if (fieldscale_index >= 0) {
        AGC_FIELDSCALE_PARAMS params = config->fieldscale;
        const FIELDSCALE_CFG_ITEM *item = &fieldscale_items[fieldscale_index];
        if (cfg_parse_long(value, &int_value) != 0 ||
            int_value < item->minimum || int_value > item->maximum) return -1;
        *cfg_fieldscale_value(&params, (unsigned int)fieldscale_index) = (uint32_t)int_value;
        if (agc_fieldscale_validate_params(&params) != 0) return -1;
        config->fieldscale = params;
        return 0;
    }

    if (strcmp(key, "dde_enable") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 0 || int_value > 1) return -1;
        config->dde.enable = (int)int_value;
        return 0;
    }
    if (strcmp(key, "dde_sigma_s") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 1 || int_value > 256) return -1;
        config->dde.sigma_s = (int)int_value;
        return 0;
    }
    if (strcmp(key, "dde_sigma_r") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 1 || int_value > 255) return -1;
        config->dde.sigma_r = (int)int_value;
        return 0;
    }
    if (strcmp(key, "dde_gain_percent") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 0 || int_value > 1000) return -1;
        config->dde.gain_percent = (int)int_value;
        return 0;
    }
    if (strcmp(key, "dde_num_iters") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 1 || int_value > 8) return -1;
        config->dde.num_iters = (int)int_value;
        return 0;
    }
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
	} else if (strcmp(key, "liveview_mirror") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    (int_value != 0 && int_value != 1)) return -1;
		config->liveview_mirror = (int)int_value;
	} else if (strcmp(key, "clahe_enable") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || (int_value != 0 && int_value != 1)) return -1;
        config->clahe.enable = (int)int_value;
    } else if (strcmp(key, "clahe_tile_size") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 ||
            (int_value != 8 && int_value != 16 && int_value != 32)) return -1;
        config->clahe.tile_size = (int)int_value;
    } else if (strcmp(key, "clahe_clip_limit") == 0) {
        if (cfg_parse_long(value, &int_value) != 0 || int_value < 4 || int_value > 128) return -1;
        config->clahe.clip_limit = (int)int_value;
	} else if (strcmp(key, "pseudo_color_mode") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value < 0 || int_value > 3) return -1;
		config->pseudo_color_mode = (int)int_value;
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
	} else if (strcmp(key, "ffc_correction") == 0) {
		if (cfg_parse_long(value, &int_value) != 0) return -1;
		config->ffc_correction = int_value ? 1 : 0;
	} else if (strcmp(key, "ffc_startup_enable") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    (int_value != 0 && int_value != 1)) return -1;
		config->ffc_startup_enable = (int)int_value;
	} else if (strcmp(key, "shutter_interval_sec") == 0 ||
		   strcmp(key, "ffc_interval_sec") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value < 0 || int_value > INT_MAX) return -1;
		config->shutter_interval_sec = (unsigned int)int_value;
	} else if (strcmp(key, "shutter_close_wait_ms") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 || int_value <= 0 || int_value > INT_MAX) return -1;
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
	} else if (strcmp(key,
			   "agc_histogram_p1_p99_temporal_previous_percent") == 0) {
		AGC_HISTOGRAM_P1_P99_PARAMS params = config->histogram_p1_p99;
		if (cfg_parse_long(value, &int_value) != 0 ||
		    int_value < 0 || int_value > 99) {
			return -1;
		}
		params.temporal_previous_percent = (uint32_t)int_value;
		config->histogram_p1_p99 = params;
	} else if (strcmp(key, "agc_histogram_p1_p99_percentile_low") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    int_value < 0 || int_value > 99) return -1;
		config->histogram_p1_p99.percentile_low = (uint32_t)int_value;
	} else if (strcmp(key, "agc_histogram_p1_p99_percentile_high") == 0) {
		if (cfg_parse_long(value, &int_value) != 0 ||
		    int_value < 1 || int_value > 100) return -1;
		config->histogram_p1_p99.percentile_high = (uint32_t)int_value;
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

	if (agc_histogram_p1_p99_set_params(&config->histogram_p1_p99) != 0 ||
	    agc_fieldscale_set_params(&config->fieldscale) != 0) {
		return -1;
	}

	if (liveview_set_fps(config->fps) != 0) {
		apply_result = -1;
	}
	if (liveview_set_output_mode(config->output_mode) != 0) {
		apply_result = -1;
	}
	liveview_set_mirror(config->liveview_mirror);
	if (pseudo_set_mode(config->pseudo_color_mode) != 0) apply_result = -1;
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

	ffc_set_shutter_interval_sec(config->shutter_interval_sec);
	liveview_set_shutter_close_wait_ms(config->shutter_close_wait_ms);
	ffc_set_enabled(config->ffc_correction);
	ffc_set_startup_enabled(config->ffc_startup_enable);
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
	if (clahe_set_params(&config->clahe) != 0) apply_result = -1;
    if (dde_set_params(&config->dde) != 0) apply_result = -1;
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

	/* Validate the pair after parsing so CFG key order does not matter. */
	if (agc_histogram_p1_p99_validate_params(&loaded_config.histogram_p1_p99) != 0) {
		printf("Invalid Histogram percentiles: require 0 <= low < high <= 100; keep previous Histogram settings\n");
		loaded_config.histogram_p1_p99 = g_app_config.histogram_p1_p99;
	}
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
    {
        int i = cfg_fieldscale_index(key);
        if (i >= 0) {
            *value = (int)*cfg_fieldscale_value(&g_app_config.fieldscale, (unsigned int)i);
            return 0;
        }
    }
    if (strcmp(key, "dde_enable") == 0) { *value = g_app_config.dde.enable; return 0; }
    if (strcmp(key, "dde_sigma_s") == 0) { *value = g_app_config.dde.sigma_s; return 0; }
    if (strcmp(key, "dde_sigma_r") == 0) { *value = g_app_config.dde.sigma_r; return 0; }
    if (strcmp(key, "dde_gain_percent") == 0) { *value = g_app_config.dde.gain_percent; return 0; }
    if (strcmp(key, "dde_num_iters") == 0) { *value = g_app_config.dde.num_iters; return 0; }
	if (strcmp(key, "cfg_boot_load") == 0) {
		*value = g_app_config.boot_load;
	} else if (strcmp(key, "fps") == 0) {
		*value = (int)g_app_config.fps;
	} else if (strcmp(key, "output_mode") == 0) {
		*value = (int)g_app_config.output_mode;
	} else if (strcmp(key, "liveview_mirror") == 0) {
		*value = g_app_config.liveview_mirror;
	} else if (strcmp(key, "clahe_enable") == 0) {
        *value = g_app_config.clahe.enable;
    } else if (strcmp(key, "clahe_tile_size") == 0) {
        *value = g_app_config.clahe.tile_size;
    } else if (strcmp(key, "clahe_clip_limit") == 0) {
        *value = g_app_config.clahe.clip_limit;
	} else if (strcmp(key, "pseudo_color_mode") == 0) {
		*value = g_app_config.pseudo_color_mode;
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
	} else if (strcmp(key, "ffc_correction") == 0) {
		*value = g_app_config.ffc_correction;
	} else if (strcmp(key, "ffc_startup_enable") == 0) {
		*value = g_app_config.ffc_startup_enable;
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
	} else if (strcmp(key,
			  "agc_histogram_p1_p99_temporal_previous_percent") == 0) {
		*value = (int)g_app_config.histogram_p1_p99.temporal_previous_percent;
	} else if (strcmp(key, "agc_histogram_p1_p99_percentile_low") == 0) {
		*value = (int)g_app_config.histogram_p1_p99.percentile_low;
	} else if (strcmp(key, "agc_histogram_p1_p99_percentile_high") == 0) {
		*value = (int)g_app_config.histogram_p1_p99.percentile_high;
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

static void app_config_request_agc(void)
{
    AGC_MODE mode = AGC_MODE_OFF;
    if (g_app_config.agc_histogram_p1_p99_enable) mode = AGC_MODE_HISTOGRAM_P1_P99_CPU;
    else if (g_app_config.agc_min_max_tone_curve_enable) mode = AGC_MODE_MIN_MAX_TONE_CURVE_ISP;
    else if (g_app_config.agc_fieldscale_enable) mode = AGC_MODE_FIELDSCALE_CPU;
    if (agc_mode_request(mode) == 0)
        printf("AGC saved; on/off applies on next RAW frame\n");
    else
        printf("AGC saved; selecting a different algorithm requires Liveview Restart\n");
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
	if (ret != 0 ||
	    agc_histogram_p1_p99_validate_params(&g_app_config.histogram_p1_p99) != 0) {
		printf("invalid CFG setting %s=%s\n", key, value_text);
		g_app_config = old_config;
		return -1;
	}
	if (app_config_save() != 0) {
		g_app_config = old_config;
		return -1;
	}

    if (strcmp(key, "ffc_correction") == 0) {
        ret = liveview_set_ffc_enabled(g_app_config.ffc_correction);
        if (ret < 0) {
            printf("FFC CFG saved, but runtime apply failed\n");
            return ret;
        }
        if (ret == 1) app_config_print_restart_required();
        else if (ret == 2) printf("FFC CFG saved; waiting for calibration to enable compensation\n");
        else printf("FFC compensation saved and applied immediately\n");
    } else if (strcmp(key, "shutter_close_wait_ms") == 0) {
        ret = liveview_apply_shutter_close_wait_ms(g_app_config.shutter_close_wait_ms);
        if (ret < 0) return ret;
        if (ret == 1) app_config_print_restart_required();
        else printf("Shutter close wait saved; next FFC uses the new value\n");
    } else if (strcmp(key, "shutter_interval_sec") == 0 || strcmp(key, "ffc_interval_sec") == 0) {
        ret = liveview_set_shutter_interval_sec(g_app_config.shutter_interval_sec);
        if (ret < 0) return ret;
        if (ret == 1) app_config_print_restart_required();
        else printf("FFC interval saved and applied immediately (0 disables periodic calibration)\n");
    } else if (strcmp(key, "bpr_correction") == 0 || strcmp(key, "nuc_correction") == 0) {
        int is_bpr = strcmp(key, "bpr_correction") == 0;
        const char *name = is_bpr ? "BPR" : "NUC";
        ret = is_bpr ? liveview_set_bpr_enabled(g_app_config.bpr_correction) :
                       liveview_set_nuc_enabled(g_app_config.nuc_correction);
        if (ret < 0) {
            printf("%s CFG saved, but runtime apply failed; check runtime status\n", name);
            return ret;
        }
        if (ret == 1) {
            printf("%s CFG saved; Liveview is stopped\n", name);
            app_config_print_restart_required();
        } else {
            printf("%s on/off saved and applied (no restart)\n", name);
        }
    } else if (strcmp(key, "bpr_threshold") == 0) {
        bpr_set_threshold(g_app_config.bpr_threshold);
        printf("BPR threshold saved and applied; next Bright/Dark calibration uses it (no restart)\n");
    } else if (strncmp(key, "dde_", 4) == 0) {
        ret = dde_set_params(&g_app_config.dde);
        if (ret != 0) return ret;
        printf("DDE saved; applies on next display frame (no restart)\n");
    } else if (strcmp(key, "clahe_enable") == 0 || strcmp(key, "clahe_tile_size") == 0 ||
        strcmp(key, "clahe_clip_limit") == 0) {
        ret = clahe_set_params(&g_app_config.clahe);
        if (ret != 0) return ret;
        printf("CLAHE saved; applies on next display frame (no restart)\n");
	} else if (strcmp(key,
			  "agc_histogram_p1_p99_temporal_previous_percent") == 0 ||
		   strcmp(key, "agc_histogram_p1_p99_percentile_low") == 0 ||
		   strcmp(key, "agc_histogram_p1_p99_percentile_high") == 0) {
		ret = agc_histogram_p1_p99_set_params(
			&g_app_config.histogram_p1_p99);
		if (ret != 0) return ret;
		printf("Histogram P1/P99 parameters saved and applied; next Histogram frame resets temporal history (no restart)\n");
	} else if (strcmp(key, "agc_histogram_p1_p99_enable") == 0 ||
        strcmp(key, "agc_min_max_tone_curve_enable") == 0 ||
        strcmp(key, "agc_fieldscale_enable") == 0) {
        app_config_request_agc();
    } else if (strcmp(key, "pseudo_color_mode") == 0) {
        ret = pseudo_request_mode(g_app_config.pseudo_color_mode);
        if (ret != 0) return ret;
        printf("Pseudo color saved; applies on next display frame (no restart)\n");
    } else if (cfg_fieldscale_index(key) >= 0) {
        ret = agc_fieldscale_set_params(&g_app_config.fieldscale);
        if (ret != 0) return ret;
        printf("Fieldscale parameters saved and applied; next Fieldscale frame uses new values (no restart)\n");
    } else {
        app_config_print_restart_required();
    }
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

	printf("  5. %-31s = %d (%s)\n", "liveview_mirror",
	       g_app_config.liveview_mirror,
	       g_app_config.liveview_mirror ? "on" : "off");

	printf("  6. %-31s = %d (%s)\n", "pseudo_color_mode",
	       g_app_config.pseudo_color_mode, pseudo_mode_name(g_app_config.pseudo_color_mode));
	printf("\n[BPR]\n");
	printf("  1. %-31s = %d (%s)\n", "bpr_correction",
	       g_app_config.bpr_correction,
	       g_app_config.bpr_correction ? "on" : "off");
	printf("  2. %-31s = %u RAW8 DN\n", "bpr_threshold",
	       g_app_config.bpr_threshold);

	printf("\n[Shutter / FFC]\n");
	printf("  1. %-31s = %d (%s)\n", "ffc_correction",
	       g_app_config.ffc_correction,
	       g_app_config.ffc_correction ? "on" : "off");
	printf("  2. %-31s = %u sec\n", "shutter_interval_sec",
	       g_app_config.shutter_interval_sec);
	printf("  3. %-31s = %u ms\n", "shutter_close_wait_ms",
	       g_app_config.shutter_close_wait_ms);
	printf("  4. %-31s = %d (%s)\n", "ffc_startup_enable",
	       g_app_config.ffc_startup_enable,
	       g_app_config.ffc_startup_enable ? "on" : "off");

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

	printf("\n[AGC]\n");
	printf("  1. %-31s = %d (%s)\n",
	       "agc_histogram_p1_p99_enable",
	       g_app_config.agc_histogram_p1_p99_enable,
	       g_app_config.agc_histogram_p1_p99_enable ? "on" : "off");
	printf("       * %-27s = %u\n",
	       "agc_histogram_p1_p99_temporal_previous_percent",
	       g_app_config.histogram_p1_p99.temporal_previous_percent);
	printf("       * %-27s = %u\n",
	       "agc_histogram_p1_p99_percentile_low",
	       g_app_config.histogram_p1_p99.percentile_low);
	printf("       * %-27s = %u\n",
	       "agc_histogram_p1_p99_percentile_high",
	       g_app_config.histogram_p1_p99.percentile_high);

	printf("  2. %-31s = %d (%s)\n",
	       "agc_min_max_tone_curve_enable",
	       g_app_config.agc_min_max_tone_curve_enable,
	       g_app_config.agc_min_max_tone_curve_enable ? "on" : "off");

	printf("  3. %-31s = %d (%s)\n",
	       "agc_fieldscale_enable",
	       g_app_config.agc_fieldscale_enable,
	       g_app_config.agc_fieldscale_enable ? "on" : "off");

	for (point = 0; point < FIELDSCALE_CFG_COUNT; point++) {
		printf("       * %-27s = %u\n",
		       fieldscale_items[point].key,
		       *cfg_fieldscale_value(&g_app_config.fieldscale, point));
	}

	printf("  4. %-31s = %s\n",
	       "selected_runtime_agc",
	       agc_mode_name(agc_mode_get()));

	printf("\n[DDE]\n");
    printf("  dde_enable=%d\n", g_app_config.dde.enable);
    printf("  dde_sigma_s=%d\n", g_app_config.dde.sigma_s);
    printf("  dde_sigma_r=%d\n", g_app_config.dde.sigma_r);
    printf("  dde_gain_percent=%d\n", g_app_config.dde.gain_percent);
    printf("  dde_num_iters=%d\n", g_app_config.dde.num_iters);
	printf("\n[CLAHE]\n");
	printf("  1. %-31s = %d (%s)\n", "clahe_enable",
		g_app_config.clahe.enable,
		g_app_config.clahe.enable ? "on" : "off");

	printf("  2. %-31s = %u\n", "clahe_tile_size",
		g_app_config.clahe.tile_size);

	printf("  3. %-31s = %u\n", "clahe_clip_limit",
		g_app_config.clahe.clip_limit);

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
		printf("BPR on/off and threshold apply immediately.\n");
		printf("The threshold affects the next Bright/Dark calibration, not the existing DPC table.\n");
		printf(" 1. BPR on/off\n");
		printf(" 2. Bright/Dark BPR threshold (RAW8 DN)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("runtime: BPR=%s, threshold=%u RAW8 DN\n",
		       bpr_is_enabled() ? "on" : "off", bpr_get_threshold());
		printf("pending CFG: BPR=%s, threshold=%u RAW8 DN\n",
		       g_app_config.bpr_correction ? "on" : "off",
		       g_app_config.bpr_threshold);

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
		printf(" 1. FFC on/off (apply immediately)\n");
		printf(" 2. FFC interval (0 = periodic off)\n");
		printf(" 3. Shutter close wait time (next calibration)\n");
		printf(" 4. FFC at startup on/off\n");
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
		printf("runtime: FFC at startup=%s; pending CFG=%s\n",
		       ffc_is_startup_enabled() ? "on" : "off",
		       g_app_config.ffc_startup_enable ? "on" : "off");
		printf("Startup FFC runs once independently of FFC on/off; applies on restart or reboot.\n");
		printf("Shutter interval: 0 disables periodic FFC; startup/manual FFC and correction remain available.\n");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool("FFC", "ffc_correction",
				g_app_config.ffc_correction);
			break;
		case 2:
			ret = app_config_prompt_int("FFC interval (0 = periodic off)",
				"shutter_interval_sec",
				(int)g_app_config.shutter_interval_sec,
				0, INT_MAX, " sec");
			break;
		case 3:
			ret = app_config_prompt_int("Shutter close wait time",
				"shutter_close_wait_ms",
				(int)g_app_config.shutter_close_wait_ms,
				1, INT_MAX, " ms");
			break;
		case 4:
			ret = app_config_prompt_bool("FFC at startup", "ffc_startup_enable",
				g_app_config.ffc_startup_enable);
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
		printf("NUC on/off applies immediately; table interval requires restart.\n");
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

static int app_config_apply_fieldscale_preset(const char *name,
		unsigned int les_threshold, unsigned int message_iterations)
{
	APP_CONFIG_VALUES old_config;
	AGC_FIELDSCALE_PARAMS *params;

	cfg_initialize_from_runtime();
	old_config = g_app_config;
	params = &g_app_config.fieldscale;
	params->grid_size = 8U;
	params->les_distance = 2U;
	params->les_threshold = les_threshold;
	params->message_distance = 1U;
	params->message_iterations = message_iterations;
	params->temporal_previous_percent = 90U;
	if (agc_fieldscale_validate_params(params) != 0) {
		printf("Invalid %s Fieldscale preset\n", name);
		g_app_config = old_config;
		return -1;
	}
	if (app_config_save() != 0) {
		g_app_config = old_config;
		return -1;
	}
	if (agc_fieldscale_set_params(params) != 0) {
		return -1;
	}

	printf("%s Fieldscale preset saved and applied; next Fieldscale frame uses it (no restart)\n",
	       name);
	return 0;
}

static int app_config_histogram_p1_p99_menu_run(void)
{
	int option;
	int ret = 0;

	while (1) {
		AGC_HISTOGRAM_P1_P99_PARAMS runtime;

		agc_histogram_p1_p99_get_params(&runtime);
		printf("----------------------------------------\n");
		printf("Histogram P1/P99 AGC (CPU) setting\n");
		printf(" 1. Histogram P1/P99 on/off: CFG=%s, runtime=%s\n",
		       g_app_config.agc_histogram_p1_p99_enable ? "on" : "off",
		       agc_histogram_p1_p99_is_enabled() ? "on" : "off");
		printf(" 2. Temporal Smoothing (previous %%, 0: off): CFG=%u, runtime=%u\n",
		       g_app_config.histogram_p1_p99.temporal_previous_percent,
		       runtime.temporal_previous_percent);
		printf(" 3. Low percentile P (0..99): CFG=%u, runtime=%u\n",
		       g_app_config.histogram_p1_p99.percentile_low, runtime.percentile_low);
		printf(" 4. High percentile P (1..100): CFG=%u, runtime=%u\n",
		       g_app_config.histogram_p1_p99.percentile_high, runtime.percentile_high);
		printf("Low P must be below High P; P0/P100 uses min/max.\n");
		printf(" 0. Back\n");
		printf("Smoothing is applied to P1/P99 thresholds, not output pixels.\n");
		printf("Parameter changes apply on the next Histogram frame and reset temporal history; no restart.\n");
		printf("Current AGC on/off applies on next RAW frame; changing algorithm requires Restart.\n");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_prompt_bool(
				"Histogram P1/P99 AGC (CPU)",
				"agc_histogram_p1_p99_enable",
				g_app_config.agc_histogram_p1_p99_enable);
			break;
		case 2:
			ret = app_config_prompt_int(
				"Temporal Smoothing (previous %, 0: off)",
				"agc_histogram_p1_p99_temporal_previous_percent",
				(int)g_app_config.histogram_p1_p99.temporal_previous_percent,
				0, 99, " %");
			break;
		case 3:
			ret = app_config_prompt_int("Low percentile P",
				"agc_histogram_p1_p99_percentile_low",
				(int)g_app_config.histogram_p1_p99.percentile_low,
				0, (int)g_app_config.histogram_p1_p99.percentile_high - 1, " %");
			break;
		case 4:
			ret = app_config_prompt_int("High percentile P",
				"agc_histogram_p1_p99_percentile_high",
				(int)g_app_config.histogram_p1_p99.percentile_high,
				(int)g_app_config.histogram_p1_p99.percentile_low + 1, 100, " %");
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

static int app_config_fieldscale_menu_run(void)
{
    int option;
    int ret = 0;
    unsigned int i;
    while (1) {
        AGC_FIELDSCALE_PARAMS runtime;
        agc_fieldscale_get_params(&runtime);
        printf("----------------------------------------\nFieldscale Local Adaptive AGC (CPU) setting\n");
        printf(" 1. Fieldscale on/off: CFG=%s, runtime=%s\n",
               g_app_config.agc_fieldscale_enable ? "on" : "off",
               agc_fieldscale_is_enabled() ? "on" : "off");
		printf(" 2. Standard: grid=8, LES=2, TLES=400, MP distance=1, N=7, current/previous=10/90%%\n");
		printf(" 3. Fast: grid=8, LES=2, TLES=800, MP distance=1, N=1, current/previous=10/90%%\n");
        for (i = 0; i < FIELDSCALE_CFG_COUNT; i++) {
            printf(" %u. %s: CFG=%u, runtime=%u\n", i + 4,
                   fieldscale_items[i].label,
                   *cfg_fieldscale_value(&g_app_config.fieldscale, i),
                   *cfg_fieldscale_value(&runtime, i));
        }
        printf(" 0. Back\nParameter changes apply on the next Fieldscale frame; no restart.\n");
        printf("Current AGC on/off applies on next RAW frame; changing algorithm requires Restart.\n");
        printf("Distances are clipped at grid boundaries. Parameter changes reset temporal history.\n");
        printf("Fieldscale runtime: %s\n", agc_fieldscale_is_enabled() ? "on" : "off");
        option = app_config_read_option();
        if (option == 0 || option == -1) return ret;
        if (option == MENU_OPTION_REFRESH) continue;
        if (option == 1) {
            ret = app_config_prompt_bool("Fieldscale Local Adaptive AGC (CPU)",
                "agc_fieldscale_enable", g_app_config.agc_fieldscale_enable);
            continue;
        }
		if (option == 2) {
			ret = app_config_apply_fieldscale_preset("Standard", 400U, 7U);
			continue;
		}
		if (option == 3) {
			ret = app_config_apply_fieldscale_preset("Fast", 800U, 1U);
			continue;
		}
        if (option < 4 || option > (int)FIELDSCALE_CFG_COUNT + 3) {
            printf("Invalid option\n");
            continue;
        }
        i = (unsigned int)option - 4U;
        ret = app_config_prompt_int(fieldscale_items[i].label,
            fieldscale_items[i].key,
            (int)*cfg_fieldscale_value(&g_app_config.fieldscale, i),
            fieldscale_items[i].minimum, fieldscale_items[i].maximum, "");
    }
}

static int app_config_agc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;
	APP_CONFIG_VALUES old_config;

	while (running) {
		printf("----------------------------------------\n");
		printf("AGC setting (mutually exclusive)\n");
		printf(" 1. Histogram P1/P99 AGC (CPU) setting\n");
		printf(" 2. Min/Max Tone Curve AGC (ISP) on/off\n");
		printf(" 3. Fieldscale Local Adaptive AGC (CPU) setting\n");
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
		printf("Current AGC on/off: next RAW frame. Different algorithm: Restart.\n");
		printf("pipeline: after RAW snapshot, before VideoProc\n");

		option = app_config_read_option();
		switch (option) {
		case 1:
			ret = app_config_histogram_p1_p99_menu_run();
			break;
		case 2:
			ret = app_config_prompt_bool(
				"Min/Max Tone Curve AGC (ISP)",
				"agc_min_max_tone_curve_enable",
				g_app_config.agc_min_max_tone_curve_enable);
			break;
		case 3:
			ret = app_config_fieldscale_menu_run();
			break;
		case 4:
			old_config = g_app_config;
			g_app_config.agc_histogram_p1_p99_enable = 0;
			g_app_config.agc_min_max_tone_curve_enable = 0;
			g_app_config.agc_fieldscale_enable = 0;
			ret = app_config_save();
			if (ret == 0) {
				app_config_request_agc();
			} else {
				g_app_config = old_config;
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

static void app_config_set_dde_preset(DDE_PARAMS *params, int fast)
{
    params->sigma_s = 21;
    params->sigma_r = 51;
    params->gain_percent = 200;
    params->num_iters = fast ? 1 : 3;
}

static int app_config_apply_dde_preset(int fast)
{
    APP_CONFIG_VALUES old_config;
    cfg_initialize_from_runtime();
    old_config = g_app_config;
    app_config_set_dde_preset(&g_app_config.dde, fast);
    if (dde_validate_params(&g_app_config.dde) != 0 || app_config_save() != 0) {
        g_app_config = old_config;
        return -1;
    }
    if (dde_set_params(&g_app_config.dde) != 0) {
        printf("DDE preset saved, but runtime apply failed\n");
        return -1;
    }
    printf("DDE preset saved; applies on next display frame\n");
    return 0;
}

static int app_config_dde_menu_run(void)
{
    int option, ret = 0;
    for (;;) {
        DDE_PARAMS runtime;
        dde_get_params(&runtime);
        printf("----------------------------------------\nDDE setting (CPU, Y8)\n");
        printf(" 1. On/off: CFG=%s, runtime=%s\n",
            g_app_config.dde.enable ? "on" : "off", runtime.enable ? "on" : "off");
        printf(" 2. Standard preset: Sigma S=21, Sigma R=51, Detail gain=200%%, Iterations=3\n");
        printf(" 3. Fast preset: Sigma S=21, Sigma R=51, Detail gain=200%%, Iterations=1\n");
        printf(" 4. Sigma S (pixels): CFG=%d, runtime=%d\n", g_app_config.dde.sigma_s, runtime.sigma_s);
        printf(" 5. Sigma R (Y8 DN): CFG=%d, runtime=%d\n", g_app_config.dde.sigma_r, runtime.sigma_r);
        printf(" 6. Detail gain (%%): CFG=%d, runtime=%d\n", g_app_config.dde.gain_percent, runtime.gain_percent);
        printf(" 7. Iterations: CFG=%d, runtime=%d\n", g_app_config.dde.num_iters, runtime.num_iters);
        printf(" 0. Back\nPresets change Sigma S, Sigma R, Detail gain and Iterations. Parameters apply on next display frame.\n");
        option = app_config_read_option();
        switch (option) {
        case 1:
            ret = app_config_prompt_bool("DDE", "dde_enable", g_app_config.dde.enable);
            break;
        case 2: ret = app_config_apply_dde_preset(0); break;
        case 3: ret = app_config_apply_dde_preset(1); break;
        case 4:
            ret = app_config_prompt_int("Sigma S (pixels)", "dde_sigma_s", g_app_config.dde.sigma_s, 1, 256, "");
            break;
        case 5:
            ret = app_config_prompt_int("Sigma R (Y8 DN)", "dde_sigma_r", g_app_config.dde.sigma_r, 1, 255, "");
            break;
        case 6:
            ret = app_config_prompt_int("Detail gain (%)", "dde_gain_percent", g_app_config.dde.gain_percent, 0, 1000, "");
            break;
        case 7:
            ret = app_config_prompt_int("Iterations", "dde_num_iters", g_app_config.dde.num_iters, 1, 8, "");
            break;
        case 0: case -1: return ret;
        case MENU_OPTION_REFRESH: break;
        default: printf("Invalid option\n"); break;
        }
    }
}

static int app_config_clahe_menu_run(void)
{
	APP_CONFIG_VALUES old_config;
    int option, value, ret = 0;
    for (;;) {
        printf("----------------------------------------\nCLAHE setting (CPU, Y8)\n");
        printf(" 1. On/off: %s\n", g_app_config.clahe.enable ? "on" : "off");
        printf(" 2. Standard preset: Tile: 16x16 pixels, Clip: 8\n");
        printf(" 3. Fast preset: Tile: 32x32 pixels, Clip: 16\n");
        printf(" 4. Tile Size: %dx%d pixels\n",
            g_app_config.clahe.tile_size, g_app_config.clahe.tile_size);
        printf(" 5. Clip Limit: %d\n", g_app_config.clahe.clip_limit);
        printf(" 0. Back\n");
        printf("Changes apply on next display frame; no restart.\n");
        option = app_config_read_option();
        switch (option) {
        case 1:
            ret = app_config_prompt_bool("CLAHE", "clahe_enable", g_app_config.clahe.enable);
            break;
        case 2:
			cfg_initialize_from_runtime();
			old_config = g_app_config;
			g_app_config.clahe.tile_size = 16;
			g_app_config.clahe.clip_limit = 8;
			ret = app_config_save();
			if (ret == 0) {
				ret = clahe_set_params(&g_app_config.clahe);
				if (ret == 0) {
					printf("Standard CLAHE preset saved; applies on next display frame (no restart)\n");
				}
			} else {
				g_app_config = old_config;
			}
			break;
        case 3:
			cfg_initialize_from_runtime();
			old_config = g_app_config;
			g_app_config.clahe.tile_size = 32;
			g_app_config.clahe.clip_limit = 16;
			ret = app_config_save();
			if (ret == 0) {
				ret = clahe_set_params(&g_app_config.clahe);
				if (ret == 0) {
					printf("Fast CLAHE preset saved; applies on next display frame (no restart)\n");
				}
			} else {
				g_app_config = old_config;
			}
			break;
        case 4:
            printf("Tile Size: 1. 8x8  2. 16x16  3. 32x32  0. Back\n");
            value = app_config_read_option();
            if (value >= 1 && value <= 3)
                ret = app_config_update_int("clahe_tile_size", 1 << (value + 2));
            else if (value > 3) printf("Invalid option\n");
            break;
        case 5:
            ret = app_config_prompt_int("Clip Limit (count per histogram bin)",
                "clahe_clip_limit", g_app_config.clahe.clip_limit, 4, 128, "");
            break;
        case 0: case -1: return ret;
        case MENU_OPTION_REFRESH: break;
        default: printf("Invalid option\n"); break;
        }
    }
}

static int app_config_apply_simple_display_mode(int mode)
{
	APP_CONFIG_VALUES old_config;
	const char *name;
	int ret;

	cfg_initialize_from_runtime();
	old_config = g_app_config;
	g_app_config.agc_histogram_p1_p99_enable = 0;
	g_app_config.agc_min_max_tone_curve_enable = 0;
	g_app_config.agc_fieldscale_enable = 1;

	if (mode == 1 || mode == 2) {
		g_app_config.fieldscale.grid_size = 8U;
		g_app_config.fieldscale.les_distance = 2U;
		g_app_config.fieldscale.les_threshold = mode == 1 ? 400U : 800U;
		g_app_config.fieldscale.message_distance = 1U;
		g_app_config.fieldscale.message_iterations = mode == 1 ? 7U : 1U;
		g_app_config.fieldscale.temporal_previous_percent = 90U;
		g_app_config.clahe.enable = 1;
		g_app_config.clahe.tile_size = mode == 1 ? 16 : 32;
		g_app_config.clahe.clip_limit = mode == 1 ? 8 : 16;
		g_app_config.dde.enable = 1;
		app_config_set_dde_preset(&g_app_config.dde, mode == 2);
		name = mode == 1 ? "Standard" : "Fast";
	} else if (mode == 3) {
		g_app_config.clahe.enable = 0;
		g_app_config.dde.enable = 0;
		name = "AGC only";
	} else {
		return -1;
	}

	if (agc_fieldscale_validate_params(&g_app_config.fieldscale) != 0 ||
	    dde_validate_params(&g_app_config.dde) != 0) {
		g_app_config = old_config;
		return -1;
	}
	ret = app_config_save();
	if (ret != 0) {
		g_app_config = old_config;
		return ret;
	}
	if (agc_fieldscale_set_params(&g_app_config.fieldscale) != 0 ||
	    clahe_set_params(&g_app_config.clahe) != 0 ||
	    dde_set_params(&g_app_config.dde) != 0) {
		printf("%s display mode saved, but runtime apply failed\n", name);
		return -1;
	}
	app_config_request_agc();
	printf("%s display mode saved; CLAHE/DDE apply on next display frame\n",
	       name);
	return 0;
}

static int app_config_simple_display_menu_run(void)
{
	int option;
	int ret = 0;

	for (;;) {
		printf("----------------------------------------\n");
		printf("Simple display setting\n");
		printf(" 1. Standard mode: Fieldscale AGC, CLAHE and DDE on; standard presets\n");
		printf(" 2. Fast mode: Fieldscale AGC, CLAHE and DDE on; fast presets\n");
		printf(" 3. AGC only: Fieldscale AGC on; CLAHE and DDE off\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		option = app_config_read_option();
		switch (option) {
		case 1:
		case 2:
		case 3:
			ret = app_config_apply_simple_display_mode(option);
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
		printf(" 2. Simple display setting\n");
		printf(" 3. BPR setting\n");
		printf(" 4. NUC setting\n");
		printf(" 5. FFC setting\n");
		printf(" 6. TDC setting\n");
		printf(" 7. AGC setting\n");
		printf(" 8. ISP setting\n");
		printf(" 9. CLAHE setting\n");
		printf("10. DDE setting\n");
		printf("11. Show current tiffany_thermal.cfg\n");
		printf("12. Generate tiffany_thermal.cfg\n");
		printf("13. Use CFG on next boot/restart on/off\n");
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
			ret = app_config_simple_display_menu_run();
			break;
		case 3:
			ret = app_config_bpr_menu_run();
			break;
		case 4:
			ret = app_config_nuc_menu_run();
			break;
		case 5:
			ret = app_config_shutter_ffc_menu_run();
			break;
		case 6:
			ret = app_config_tdc_menu_run();
			break;
		case 7:
			ret = app_config_agc_menu_run();
			break;
		case 8:
			ret = videoproc_isp_menu_run();
			break;
		case 9:
			ret = app_config_clahe_menu_run();
			break;
		case 10:
			ret = app_config_dde_menu_run();
			break;
		case 11:
			ret = app_config_show_file();
			break;
		case 12:
			ret = app_config_save();
			break;
		case 13:
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
	fprintf(fp, "liveview_mirror=%d\n", g_app_config.liveview_mirror);
	fprintf(fp, "pseudo_color_mode=%d\n", g_app_config.pseudo_color_mode);
    fprintf(fp, "dde_enable=%d\n", g_app_config.dde.enable);
    fprintf(fp, "dde_sigma_s=%d\n", g_app_config.dde.sigma_s);
    fprintf(fp, "dde_sigma_r=%d\n", g_app_config.dde.sigma_r);
    fprintf(fp, "dde_gain_percent=%d\n", g_app_config.dde.gain_percent);
    fprintf(fp, "dde_num_iters=%d\n", g_app_config.dde.num_iters);
	fprintf(fp, "clahe_enable=%d\nclahe_tile_size=%d\nclahe_clip_limit=%d\n",
	    g_app_config.clahe.enable, g_app_config.clahe.tile_size, g_app_config.clahe.clip_limit);
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
	fprintf(fp, "ffc_correction=%d\n", g_app_config.ffc_correction);
	fprintf(fp, "ffc_startup_enable=%d\n", g_app_config.ffc_startup_enable);
	fprintf(fp, "shutter_interval_sec=%u\n",
		g_app_config.shutter_interval_sec);
	fprintf(fp, "shutter_close_wait_ms=%u\n",
		g_app_config.shutter_close_wait_ms);
	fprintf(fp, "tdc_correction=%d\n", g_app_config.tdc_correction);
	fprintf(fp, "tdc_interval_sec=%u\n",
		g_app_config.tdc_interval_sec);
	fprintf(fp, "agc_histogram_p1_p99_enable=%d\n",
		g_app_config.agc_histogram_p1_p99_enable);
	fprintf(fp, "agc_histogram_p1_p99_temporal_previous_percent=%u\n",
		g_app_config.histogram_p1_p99.temporal_previous_percent);
	fprintf(fp, "agc_histogram_p1_p99_percentile_low=%u\n",
		g_app_config.histogram_p1_p99.percentile_low);
	fprintf(fp, "agc_histogram_p1_p99_percentile_high=%u\n",
		g_app_config.histogram_p1_p99.percentile_high);
	fprintf(fp, "agc_min_max_tone_curve_enable=%d\n",
		g_app_config.agc_min_max_tone_curve_enable);
	fprintf(fp, "agc_fieldscale_enable=%d\n",
		g_app_config.agc_fieldscale_enable);
    for (point = 0; point < FIELDSCALE_CFG_COUNT; point++) {
        fprintf(fp, "%s=%u\n", fieldscale_items[point].key,
                *cfg_fieldscale_value(&g_app_config.fieldscale, point));
    }
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
