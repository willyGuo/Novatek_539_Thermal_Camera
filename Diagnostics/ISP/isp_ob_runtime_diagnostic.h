#ifndef DIAGNOSTICS_ISP_OB_RUNTIME_DIAGNOSTIC_H
#define DIAGNOSTICS_ISP_OB_RUNTIME_DIAGNOSTIC_H

int isp_ob_runtime_diagnostic_show(void);
int isp_ob_runtime_diagnostic_run_zero_offset(
	int (*run_step_diagnostic)(void));

#endif /* DIAGNOSTICS_ISP_OB_RUNTIME_DIAGNOSTIC_H */
