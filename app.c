#include "app.h"

#include "Driver/SPI/spi.h"
#include "Driver/I2C/i2c.h"
#include "Pipeline/menu.h"
#include "Pipeline/liveview.h"

static const char g_app_version[] = "v3.0.1";

const char *app_get_version(void)
{
	return g_app_version;
}

int app_init(void)
{
	if (spi1_3_init() != 0) {
		return -1;
	}
	// Tim Add
	if (I2C_0_Init() != 0) {
		spi1_3_deinit();
		return -1;
	}
	// Tim End

	if (liveview_init() != 0) {
		I2C_0_Deinit();
		spi1_3_deinit();
		return -1;
	}

	if (liveview_start() != 0) {
		liveview_deinit();
		I2C_0_Deinit();
		spi1_3_deinit();
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
	I2C_0_Deinit();
	spi1_3_deinit();
}
