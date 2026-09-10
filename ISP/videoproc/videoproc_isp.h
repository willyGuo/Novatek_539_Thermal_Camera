#ifndef ENHANCEMENT_VIDEOPROC_ISP_H
#define ENHANCEMENT_VIDEOPROC_ISP_H

void videoproc_isp_set_runtime(int af_enable, int wdr_enable,
				       int defog_enable, int three_dnr_enable);
int videoproc_isp_is_af_enabled(void);
int videoproc_isp_is_wdr_enabled(void);
int videoproc_isp_is_defog_enabled(void);
int videoproc_isp_is_3dnr_enabled(void);
int videoproc_isp_menu_run(void);

#endif /* ENHANCEMENT_VIDEOPROC_ISP_H */
