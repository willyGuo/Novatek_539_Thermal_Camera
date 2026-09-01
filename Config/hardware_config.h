#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#define THERMAL_ISP_CONFIG_PATH "/mnt/sd/isp_ty640_0.cfg"

/*
 * Shared HDAL and Vendor ISP route ID for the thermal pipeline.
 * VCAP, VideoProc ISP, Gamma, and BPR/DPC must all use this value.
 */
extern const unsigned int g_thermal_pipeline_id;

#endif
