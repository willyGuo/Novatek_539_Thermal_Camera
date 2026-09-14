#ifndef LIVEVIEW_H
#define LIVEVIEW_H

int liveview_init(void);
int liveview_start(void);
void liveview_stop(void);
int liveview_restart(void);
int liveview_is_running(void);
int liveview_get_result(void);
int liveview_get_shutter_fault(void);
/* Returns 0 when applied, 1 when stopped (CFG only), -1 on failure. */
int liveview_set_bpr_enabled(int enabled);
int liveview_set_nuc_enabled(int enabled);
void liveview_set_mirror(int enabled);
int liveview_get_mirror(void);
int liveview_set_fps(unsigned int fps);
unsigned int liveview_get_fps(void);
int liveview_set_output_mode(unsigned int mode);
unsigned int liveview_get_output_mode(void);
void liveview_set_startup_skip_frames(unsigned int frame_count);
unsigned int liveview_get_startup_skip_frames(void);
void liveview_set_runtime_skip_frames(unsigned int frame_count);
unsigned int liveview_get_runtime_skip_frames(void);
void liveview_set_shutter_close_wait_ms(unsigned int wait_ms);
unsigned int liveview_get_shutter_close_wait_ms(void);
int liveview_menu_run(void);
int liveview_run_isp_raw_to_y_step_diagnostic(void);
int liveview_run_isp_raw12_to_y_step_diagnostic(void);
int liveview_run_isp_raw14_fine_step_diagnostic(void);
void liveview_deinit(void);

#endif
