/*
 * File Name : gamma.h
 * Author : Philip Chen
 * E-mail : Philip6_Chen@pegatroncorp.com
 * Date : 2025-01-09
 * MODIFICATION HISTORY:
 *
 * Ver   Who  Date     Changes
 * ----- ---- -------- -------------------------------------------------------
 */

#ifndef ENHANCEMENT_GAMMA_H
#define ENHANCEMENT_GAMMA_H

#include "hdal.h"

HD_RESULT gamma_init_from_isp(void);
HD_RESULT gamma_apply(void);
void gamma_set_enabled(int enable);
int gamma_is_enabled(void);
void gamma_set_mode(UINT32 mode);
UINT32 gamma_get_mode(void);
void gamma_set_auto_sel(UINT32 auto_sel);
UINT32 gamma_get_auto_sel(void);
void gamma_set_load_bin(int enable);
int gamma_get_load_bin(void);
int gamma_menu_run(void);

#endif /* ENHANCEMENT_GAMMA_H */
