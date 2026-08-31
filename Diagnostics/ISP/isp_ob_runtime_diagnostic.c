#include "isp_ob_runtime_diagnostic.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hdal.h"
#include "vendor_isp.h"

#include "../../Config/hardware_config.h"

static void isp_ob_print_values(const char *name, const UINT32 *values)
{
	printf("  %s={%u, %u, %u, %u, %u}\n",
	       name,
	       (unsigned int)values[0], (unsigned int)values[1],
	       (unsigned int)values[2], (unsigned int)values[3],
	       (unsigned int)values[4]);
}

static int isp_ob_get(IQT_OB_PARAM *ob, IQT_OB_MODE_MANUAL *manual)
{
	HD_RESULT ret;

	if (ob == NULL || manual == NULL) {
		return -1;
	}

	memset(ob, 0, sizeof(*ob));
	memset(manual, 0, sizeof(*manual));
	ob->id = g_thermal_pipeline_id;
	manual->id = g_thermal_pipeline_id;

	ret = vendor_isp_get_iq(IQT_ITEM_OB_PARAM, ob);
	if (ret != HD_OK) {
		printf("ISP OB runtime: get IQT_ITEM_OB_PARAM failed=%d\n", ret);
		return -1;
	}
	ret = vendor_isp_get_iq(IQT_ITEM_OB_MODE_MANUAL, manual);
	if (ret != HD_OK) {
		printf("ISP OB runtime: get IQT_ITEM_OB_MODE_MANUAL failed=%d\n",
		       ret);
		return -1;
	}

	return 0;
}

static void isp_ob_print(const IQT_OB_PARAM *ob,
			 const IQT_OB_MODE_MANUAL *manual)
{
	printf("ISP OB runtime (ISP id=%u)\n", (unsigned int)ob->id);
	printf("  OB parameter: enable=%u, mode=%u\n",
	       (unsigned int)ob->ob.enable, (unsigned int)ob->ob.mode);
	isp_ob_print_values("manual cofs", ob->ob.manual_param.cofs);
	isp_ob_print_values("auto[0] cofs", ob->ob.auto_param[0].cofs);
	printf("  OB placement override: manual_enable=%u, manual_mode=%u\n",
	       (unsigned int)manual->ob_mode_manual.manual_enable,
	       (unsigned int)manual->ob_mode_manual.manual_mode);
	printf("  stage override enable: SIE=%u, PRE_F=%u, PRE=%u, "
	       "IFE_F=%u, IFE=%u\n",
	       (unsigned int)manual->ob_mode_manual.sie_enable,
	       (unsigned int)manual->ob_mode_manual.pre_f_enable,
	       (unsigned int)manual->ob_mode_manual.pre_enable,
	       (unsigned int)manual->ob_mode_manual.ife_f_enable,
	       (unsigned int)manual->ob_mode_manual.ife_enable);
	printf("  SIE value=%u\n",
	       (unsigned int)manual->ob_mode_manual.sie_value);
	isp_ob_print_values("PRE_F values",
			    manual->ob_mode_manual.pre_f_value);
	isp_ob_print_values("PRE values", manual->ob_mode_manual.pre_value);
	isp_ob_print_values("IFE_F values",
			    manual->ob_mode_manual.ife_f_value);
	isp_ob_print_values("IFE values", manual->ob_mode_manual.ife_value);
}

int isp_ob_runtime_diagnostic_show(void)
{
	IQT_OB_PARAM ob;
	IQT_OB_MODE_MANUAL manual;

	if (isp_ob_get(&ob, &manual) != 0) {
		return -1;
	}
	isp_ob_print(&ob, &manual);
	return 0;
}

static int isp_ob_values_are_zero(const UINT32 *values, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (values[i] != 0U) {
			return 0;
		}
	}
	return 1;
}

static int isp_ob_auto_values_are_zero(const IQT_OB_PARAM *ob)
{
	size_t i;

	for (i = 0;
	     i < sizeof(ob->ob.auto_param) / sizeof(ob->ob.auto_param[0]);
	     i++) {
		if (!isp_ob_values_are_zero(ob->ob.auto_param[i].cofs,
				sizeof(ob->ob.auto_param[i].cofs) /
				sizeof(ob->ob.auto_param[i].cofs[0]))) {
			return 0;
		}
	}
	return 1;
}

static int isp_ob_is_forced_zero(const IQT_OB_PARAM *ob,
				 const IQT_OB_MODE_MANUAL *manual)
{
	return !ob->ob.enable &&
		isp_ob_values_are_zero(ob->ob.manual_param.cofs,
			sizeof(ob->ob.manual_param.cofs) /
			sizeof(ob->ob.manual_param.cofs[0])) &&
		isp_ob_auto_values_are_zero(ob) &&
		manual->ob_mode_manual.sie_enable &&
		manual->ob_mode_manual.pre_f_enable &&
		manual->ob_mode_manual.pre_enable &&
		manual->ob_mode_manual.ife_f_enable &&
		manual->ob_mode_manual.ife_enable &&
		manual->ob_mode_manual.sie_value == 0U &&
		isp_ob_values_are_zero(manual->ob_mode_manual.pre_f_value,
			sizeof(manual->ob_mode_manual.pre_f_value) /
			sizeof(manual->ob_mode_manual.pre_f_value[0])) &&
		isp_ob_values_are_zero(manual->ob_mode_manual.pre_value,
			sizeof(manual->ob_mode_manual.pre_value) /
			sizeof(manual->ob_mode_manual.pre_value[0])) &&
		isp_ob_values_are_zero(manual->ob_mode_manual.ife_f_value,
			sizeof(manual->ob_mode_manual.ife_f_value) /
			sizeof(manual->ob_mode_manual.ife_f_value[0])) &&
		isp_ob_values_are_zero(manual->ob_mode_manual.ife_value,
			sizeof(manual->ob_mode_manual.ife_value) /
			sizeof(manual->ob_mode_manual.ife_value[0]));
}

static int isp_ob_set_and_verify(IQT_OB_PARAM *ob,
				 IQT_OB_MODE_MANUAL *manual,
				 const char *operation,
				 int expect_forced_zero)
{
	IQT_OB_PARAM verify_ob;
	IQT_OB_MODE_MANUAL verify_manual;
	HD_RESULT ret;

	ret = vendor_isp_set_iq(IQT_ITEM_OB_PARAM, ob);
	if (ret != HD_OK) {
		printf("ISP OB runtime: %s OB parameter failed=%d\n",
		       operation, ret);
		return -1;
	}
	ret = vendor_isp_set_iq(IQT_ITEM_OB_MODE_MANUAL, manual);
	if (ret != HD_OK) {
		printf("ISP OB runtime: %s OB stage override failed=%d\n",
		       operation, ret);
		return -1;
	}

	if (isp_ob_get(&verify_ob, &verify_manual) != 0) {
		return -1;
	}
	printf("ISP OB runtime after %s:\n", operation);
	isp_ob_print(&verify_ob, &verify_manual);
	if (expect_forced_zero &&
	    !isp_ob_is_forced_zero(&verify_ob, &verify_manual)) {
		printf("ISP OB runtime: zero-offset readback verification failed\n");
		return -1;
	}
	return 0;
}

static void isp_ob_make_zero_offset(IQT_OB_PARAM *ob,
				    IQT_OB_MODE_MANUAL *manual)
{
	ob->ob.enable = FALSE;
	memset(ob->ob.manual_param.cofs, 0, sizeof(ob->ob.manual_param.cofs));
	memset(ob->ob.auto_param, 0, sizeof(ob->ob.auto_param));

	/* Force every available OB hardware placement to use a zero offset. */
	manual->ob_mode_manual.sie_enable = TRUE;
	manual->ob_mode_manual.pre_f_enable = TRUE;
	manual->ob_mode_manual.pre_enable = TRUE;
	manual->ob_mode_manual.ife_f_enable = TRUE;
	manual->ob_mode_manual.ife_enable = TRUE;
	manual->ob_mode_manual.sie_value = 0;
	memset(manual->ob_mode_manual.pre_f_value, 0,
	       sizeof(manual->ob_mode_manual.pre_f_value));
	memset(manual->ob_mode_manual.pre_value, 0,
	       sizeof(manual->ob_mode_manual.pre_value));
	memset(manual->ob_mode_manual.ife_f_value, 0,
	       sizeof(manual->ob_mode_manual.ife_f_value));
	memset(manual->ob_mode_manual.ife_value, 0,
	       sizeof(manual->ob_mode_manual.ife_value));
}

int isp_ob_runtime_diagnostic_run_zero_offset(
	int (*run_step_diagnostic)(void))
{
	IQT_OB_PARAM saved_ob;
	IQT_OB_MODE_MANUAL saved_manual;
	IQT_OB_PARAM zero_ob;
	IQT_OB_MODE_MANUAL zero_manual;
	int test_ret = -1;
	int restore_ret;

	if (run_step_diagnostic == NULL) {
		return -1;
	}
	if (isp_ob_get(&saved_ob, &saved_manual) != 0) {
		return -1;
	}

	printf("Saved ISP OB runtime before zero-offset A/B test:\n");
	isp_ob_print(&saved_ob, &saved_manual);
	memcpy(&zero_ob, &saved_ob, sizeof(zero_ob));
	memcpy(&zero_manual, &saved_manual, sizeof(zero_manual));
	isp_ob_make_zero_offset(&zero_ob, &zero_manual);

	if (isp_ob_set_and_verify(&zero_ob, &zero_manual,
				  "forcing zero offset", 1) == 0) {
		/* Allow the runtime IQ setting to reach the following test frame. */
		usleep(100000);
		test_ret = run_step_diagnostic();
	}

	restore_ret = isp_ob_set_and_verify(&saved_ob, &saved_manual,
					    "restoring saved state", 0);
	if (restore_ret != 0) {
		printf("WARNING: ISP OB runtime restore failed; restart Liveview "
		       "before normal use\n");
		return -1;
	}
	printf("ISP OB zero-offset A/B test finished; original runtime state "
	       "restored\n");

	return test_ret;
}
