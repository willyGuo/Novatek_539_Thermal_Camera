#include "agc_mode.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "agc_fieldscale.h"
#include "agc_weighted.h"
#include "agc_histogram_p1_p99.h"
#include "agc_min_max_tone_curve.h"

#define MENU_OPTION_REFRESH (-2)

static AGC_MODE g_agc_mode = AGC_MODE_OFF;
static int g_requested_mode = -1;
static pthread_mutex_t g_mode_lock = PTHREAD_MUTEX_INITIALIZER;

void agc_mode_begin_session(void)
{
    pthread_mutex_lock(&g_mode_lock);
    g_requested_mode = -1;
    pthread_mutex_unlock(&g_mode_lock);
}

/* The RAW worker consumes the latest selection at a frame boundary. */
int agc_mode_request(AGC_MODE mode)
{
    if (mode < AGC_MODE_OFF || mode > AGC_MODE_WEIGHTED_CPU) return -1;
    pthread_mutex_lock(&g_mode_lock);
    g_requested_mode = (int)mode;
    pthread_mutex_unlock(&g_mode_lock);
    return 0;
}

int agc_mode_take_request(void)
{
    int mode;
    pthread_mutex_lock(&g_mode_lock);
    mode = g_requested_mode;
    g_requested_mode = -1;
    pthread_mutex_unlock(&g_mode_lock);
    return mode;
}


int agc_mode_set(AGC_MODE mode)
{
	if (mode != AGC_MODE_OFF &&
	    mode != AGC_MODE_HISTOGRAM_P1_P99_CPU &&
	    mode != AGC_MODE_MIN_MAX_TONE_CURVE_ISP &&
	    mode != AGC_MODE_FIELDSCALE_CPU &&
	    mode != AGC_MODE_WEIGHTED_CPU) {
		return -1;
	}

	pthread_mutex_lock(&g_mode_lock);
	g_agc_mode = mode;
	pthread_mutex_unlock(&g_mode_lock);
	return 0;
}

AGC_MODE agc_mode_get(void)
{
	AGC_MODE mode;
	pthread_mutex_lock(&g_mode_lock);
	mode = g_agc_mode;
	pthread_mutex_unlock(&g_mode_lock);
	return mode;
}

const char *agc_mode_name(AGC_MODE mode)
{
	switch (mode) {
	case AGC_MODE_HISTOGRAM_P1_P99_CPU:
		return "Histogram P1/P99 AGC (CPU)";
	case AGC_MODE_MIN_MAX_TONE_CURVE_ISP:
		return "Min/Max Tone Curve AGC (ISP)";
	case AGC_MODE_FIELDSCALE_CPU:
		return "Fieldscale Local Adaptive AGC (CPU)";
	case AGC_MODE_WEIGHTED_CPU:
		return "Weighted Mapping AGC (CPU)";
	case AGC_MODE_OFF:
	default:
		return "off";
	}
}

static int agc_read_option(void)
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

int agc_menu_run(void)
{
	int option;
	int running = 1;
	int ret = 0;

	while (running) {
		printf("----------------------------------------\n");
		printf("AGC menu (mutually exclusive)\n");
		printf(" 1. Histogram P1/P99 (CPU)\n");
		printf(" 2. Min/Max Tone Curve (ISP)\n");
		printf(" 3. Fieldscale Local Adaptive AGC (CPU)\n");
		printf(" 4. Disable all AGC\n");
		printf(" 5. Weighted Mapping AGC (CPU, 1x1)\n");
		printf(" 0. Back\n");
		printf("----------------------------------------\n");
		printf("selected runtime AGC: %s\n",
		       agc_mode_name(agc_mode_get()));
		printf("Settings: CFG Setting -> AGC setting\n");

		option = agc_read_option();
		switch (option) {
		case 1:
			ret = agc_histogram_p1_p99_menu_run();
			break;

		case 2:
			ret = agc_min_max_tone_curve_menu_run();
			break;

		case 3:
			ret = agc_fieldscale_menu_run();
			break;

		case 5:
			ret = agc_weighted_menu_run();
			break;

		case 4:
			agc_mode_request(AGC_MODE_OFF);
			printf("AGC off requested for next RAW frame\n");
			printf("Saved CFG is unchanged; a Liveview restart may restore it\n");
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
