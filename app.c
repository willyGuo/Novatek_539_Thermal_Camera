#include "app.h"
#include "Config/hardware_config.h"

#include <stdio.h>

#include "Driver/SPI/spi.h"
#include "Driver/I2C/i2c.h"
#include "Pipeline/menu.h"
#include "Pipeline/liveview.h"

static const char g_app_version[] = "v3.9.0";

const char *app_get_version(void)
{
	return g_app_version;
}

int app_init(void)
{
	printf("Thermal sensor: %s, driver=%s, RAW=%ux%u\n",
		THERMAL_SENSOR_NAME, THERMAL_SENSOR_DRIVER,
		(unsigned int)THERMAL_SENSOR_WIDTH, (unsigned int)THERMAL_SENSOR_HEIGHT);
#if THERMAL_HAS_TY_PERIPHERALS
	if (spi5_1_init() != 0) {
		return -1;
	}
	if (spi3_1_init() != 0) {
		return -1;
	}
	if (spi1_3_init() != 0) {
		return -1;
	}

	if (I2C_0_Init() != 0) {
		spi5_1_deinit();
		spi3_1_deinit();
		spi1_3_deinit();
		return -1;
	}
#endif

	if (liveview_init() != 0) {
#if THERMAL_HAS_TY_PERIPHERALS
		I2C_0_Deinit();
		spi1_3_deinit();
#endif
		return -1;
	}

	if (liveview_start() != 0) {
		liveview_deinit();
#if THERMAL_HAS_TY_PERIPHERALS
		I2C_0_Deinit();
		spi1_3_deinit();
#endif
		return -1;
	}

	return 0;
}

int app_run(void)
{
	return menu_run();
}

void app_deinit(void)
{
	liveview_deinit();
#if THERMAL_HAS_TY_PERIPHERALS
	I2C_0_Deinit();
	spi1_3_deinit();
#endif
}
