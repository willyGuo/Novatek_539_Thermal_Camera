#include "sensor_temp.h"

#include <stdint.h>

#include "../../Driver/SPI/spi.h"
#include "../../Driver/I2C/i2c.h"

#define SENSOR_TEMP_FRAME_SIZE 4U
#define SENSOR_TEMP_ADC_SCALE 0.0000625f
#define SENSOR_TEMP_VOLTAGE_OFFSET 1.8265f
#define SENSOR_TEMP_VOLTAGE_SLOPE (-0.0046f)
#define LENS_TEMP_FRAME_SIZE 2U
#define LENS_TEMP_RAW_SIGN_BIT 0x0800U
#define LENS_TEMP_RAW_RANGE 0x1000
#define LENS_TEMP_C_PER_LSB 0.0625f

int sensor_temp_read(float *temp_c)
{
	uint8_t tx[SENSOR_TEMP_FRAME_SIZE] = {
		0xC4,			// 0xC4: Continuous mode; 0xC5: single-shot mode
		0x8B,
		0xC4,
		0x8B,
	};
	uint8_t rx[SENSOR_TEMP_FRAME_SIZE] = {0};
	uint16_t raw_value;
	float voltage;

	if (temp_c == 0)
		return -1;

	if (spi5_1_transfer(tx, rx, SENSOR_TEMP_FRAME_SIZE) != 0) {
		return -1;
	}

	raw_value = ((uint16_t)rx[0] << 8) | rx[1];
	voltage = raw_value * SENSOR_TEMP_ADC_SCALE;
	*temp_c =
		(voltage - SENSOR_TEMP_VOLTAGE_OFFSET) /
		SENSOR_TEMP_VOLTAGE_SLOPE;

	return 0;
}




int lens_temp_read(float *temp_c)
{
	if (temp_c == 0)
		return -1;

	uint8_t rx[LENS_TEMP_FRAME_SIZE] = {0};
	uint8_t reg_addr = 0x00;		// Temperature Register
	uint16_t raw_value;
	int32_t signed_raw_value;
	
	if (i2c_read(reg_addr, rx, LENS_TEMP_FRAME_SIZE) != 0) {
		return -1;
	}
	raw_value = ((uint16_t)rx[0] << 4) |
		    ((uint16_t)rx[1] >> 4);
	signed_raw_value = (int32_t)raw_value;
	if ((raw_value & LENS_TEMP_RAW_SIGN_BIT) != 0U) {
		signed_raw_value -= LENS_TEMP_RAW_RANGE;
	}
	*temp_c = (float)signed_raw_value * LENS_TEMP_C_PER_LSB;
	// printf("LENS Temp = : %f\n", *temp_c);


	return 0;
}
