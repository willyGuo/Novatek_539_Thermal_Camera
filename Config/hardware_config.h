#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

/* One compile-time selection for capture, frame layout and board peripherals.
 * Override with -DTHERMAL_SENSOR=THERMAL_SENSOR_ATTO640, or change the default.
 */
#define THERMAL_SENSOR_TY640   1
#define THERMAL_SENSOR_ATTO640 2
#ifndef THERMAL_SENSOR
#define THERMAL_SENSOR THERMAL_SENSOR_TY640
#endif

#if THERMAL_SENSOR == THERMAL_SENSOR_TY640
#define THERMAL_SENSOR_NAME "TY640"
#define THERMAL_SENSOR_DRIVER "nvt_sen_ty640"
#define THERMAL_SENSOR_WIDTH  640
#define THERMAL_SENSOR_HEIGHT 512
#define THERMAL_HAS_TY_PERIPHERALS 1
#elif THERMAL_SENSOR == THERMAL_SENSOR_ATTO640
#define THERMAL_SENSOR_NAME "ATTO640"
#define THERMAL_SENSOR_DRIVER "nvt_sen_atto640"
#define THERMAL_SENSOR_WIDTH  640
#define THERMAL_SENSOR_HEIGHT 480
/* The ATTO sample leaves TY640 SPI/I2C devices uninitialized. Sensor I2C
 * configuration belongs to nvt_sen_atto640 in the SDK, not Driver/I2C.
 */
#define THERMAL_HAS_TY_PERIPHERALS 0
#else
#error "THERMAL_SENSOR must be THERMAL_SENSOR_TY640 or THERMAL_SENSOR_ATTO640"
#endif

#define THERMAL_HAS_TEMPERATURE THERMAL_HAS_TY_PERIPHERALS
#define THERMAL_HAS_FOS THERMAL_HAS_TY_PERIPHERALS

/* Keep the ISP reload path used by both supplied samples. The file contents
 * must match the selected sensor and board; the filename is not a driver ID.
 */
#ifndef THERMAL_ISP_CONFIG_PATH
#define THERMAL_ISP_CONFIG_PATH "/mnt/sd/isp_ty640_0.cfg"
#endif

/*
 * Shared HDAL and Vendor ISP route ID for the thermal pipeline.
 * VCAP, VideoProc ISP, Gamma, and BPR/DPC must all use this value.
 */
extern const unsigned int g_thermal_pipeline_id;

#endif
