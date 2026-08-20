#ifndef BPR_H
#define BPR_H

typedef void (*BPR_CALIBRATION_STATE_CB)(const char *name, int active);

int bpr_menu_run(void);
void bpr_bind_calibration_state_callback(
	BPR_CALIBRATION_STATE_CB calibration_state);
void bpr_set_threshold(unsigned int threshold);
unsigned int bpr_get_threshold(void);
int bpr_set_enabled(int enable);
int bpr_is_enabled(void);
int bpr_load_table(void);
void bpr_set_runtime_enabled(int enable);
int bpr_is_runtime_enabled(void);
void bpr_set_runtime_threshold(unsigned int threshold);
unsigned int bpr_get_runtime_threshold(void);
int bpr_run_runtime_calibration(void);

#endif
