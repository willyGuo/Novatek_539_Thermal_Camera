/*
 * File Name : i2c.h
 * Author : Philip Chen
 * E-mail : Philip6_Chen@pegatroncorp.com
 * Date : 2025-01-09
 * MODIFICATION HISTORY:
 *
 * Ver   Who  Date     Changes
 * ----- ---- -------- -------------------------------------------------------
 */


#ifndef __I2C_H_
#define __I2C_H_

#include <stdint.h>

int I2C_0_Init(void);
void I2C_0_Deinit(void);

int i2c_read(uint8_t reg_addr, uint8_t *read_data, uint16_t len);
int i2c_write(uint8_t reg_addr, uint8_t *write_data, uint16_t len);



#endif


