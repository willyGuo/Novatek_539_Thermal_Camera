#ifndef THERMAL_VIDEOPROC_DEVICES_H
#define THERMAL_VIDEOPROC_DEVICES_H

#include "hdal.h"

/* Logical HDAL VideoProc devices share a namespace even when their engines
 * differ. isp_id (sensor IQ group or vendor VPE channel) is independent. */
#define THERMAL_RAWALL_DEVICE 0
#define THERMAL_MIRROR_DEVICE 1
#if THERMAL_RAWALL_DEVICE == THERMAL_MIRROR_DEVICE
#error "RAWALL and VPE mirror require distinct VideoProc devices"
#endif

#define THERMAL_MIRROR_CTRL HD_VIDEOPROC_CTRL(THERMAL_MIRROR_DEVICE)
#define THERMAL_MIRROR_IN   HD_VIDEOPROC_IN(THERMAL_MIRROR_DEVICE, 0)
#define THERMAL_MIRROR_OUT  HD_VIDEOPROC_OUT(THERMAL_MIRROR_DEVICE, 0)

#endif
