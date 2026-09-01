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
void gamma_reset(void);
HD_RESULT gamma_set_enabled(int enable);
int gamma_is_enabled(void);
int gamma_menu_run(void);

#endif /* ENHANCEMENT_GAMMA_H */
