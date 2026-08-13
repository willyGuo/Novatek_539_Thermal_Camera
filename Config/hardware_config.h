#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

/*
 * Shared HDAL and Vendor ISP route ID for the thermal pipeline.
 * VCAP, VideoProc ISP, Gamma, and BPR/DPC must all use this value.
 */
extern const unsigned int g_thermal_pipeline_id;

#endif
