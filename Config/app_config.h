#ifndef APP_CONFIG_H
#define APP_CONFIG_H

int app_config_load(void);
int app_config_save(void);
int app_config_menu_run(void);
int app_config_update_int(const char *key, int value);
int app_config_get_int(const char *key, int *value);
int app_config_update_videoproc_isp(int af_enable, int wdr_enable,
				    int defog_enable, int three_dnr_enable);
int app_config_update_sensor_temp_points(const float *points,
					 unsigned int point_count);
void app_config_print_restart_required(void);
int app_config_get_boot_load(void);

#endif
