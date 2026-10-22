#ifndef ENHANCEMENT_AGC_MODE_H
#define ENHANCEMENT_AGC_MODE_H

typedef enum {
	AGC_MODE_OFF = 0,
	AGC_MODE_HISTOGRAM_P1_P99_CPU,
	AGC_MODE_MIN_MAX_TONE_CURVE_ISP,
	AGC_MODE_FIELDSCALE_CPU,
	AGC_MODE_WEIGHTED_CPU
} AGC_MODE;

void agc_mode_begin_session(void);
int agc_mode_request(AGC_MODE mode);
int agc_mode_take_request(void);
int agc_mode_set(AGC_MODE mode);
AGC_MODE agc_mode_get(void);
const char *agc_mode_name(AGC_MODE mode);
int agc_menu_run(void);

#endif /* ENHANCEMENT_AGC_MODE_H */
