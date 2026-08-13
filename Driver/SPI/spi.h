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


#ifndef __SPI_H_
#define __SPI_H_

#include <stddef.h>
#include <stdint.h>

int spi1_3_init(void);
void spi1_3_deinit(void);

int spi1_3_transfer(const uint8_t *tx, uint8_t *rx, size_t len);


#endif
