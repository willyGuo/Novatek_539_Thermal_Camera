// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI testing utility (using spidev driver)
 *
 * Copyright (c) 2007  MontaVista Software, Inc.
 * Copyright (c) 2007  Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * Cross-compile with cross-gcc -I/path/to/cross-kernel/include
 */

#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/spi/spidev.h>
#include <math.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const char *spi_device = "/dev/spidev0.0";
static uint32_t mode = 1;
static uint8_t bits = 8;
static char *output_file;
static uint32_t speed = 2000000;
static uint16_t delay = 0;


static int fd = -1;


/*
 *  Unescape - process hexadecimal escape character
 *      converts shell input "\x23" -> 0x23
 */


int spi1_3_transfer(const uint8_t *tx, uint8_t *rx, size_t len)
{
	int ret;
	int out_fd;
	ssize_t write_size;
	struct spi_ioc_transfer tr = {0};

	if (fd < 0 || tx == NULL || rx == NULL || len == 0 ||
	    len > UINT32_MAX) {
		errno = EINVAL;
		perror("invalid spi transfer");
		return -1;
	}

	tr.tx_buf = (uintptr_t)tx;
	tr.rx_buf = (uintptr_t)rx;
	tr.len = (uint32_t)len;
	tr.delay_usecs = delay;
	tr.speed_hz = speed;
	tr.bits_per_word = bits;

	if (mode & SPI_TX_OCTAL)
		tr.tx_nbits = 8;
	else if (mode & SPI_TX_QUAD)
		tr.tx_nbits = 4;
	else if (mode & SPI_TX_DUAL)
		tr.tx_nbits = 2;
	if (mode & SPI_RX_OCTAL)
		tr.rx_nbits = 8;
	else if (mode & SPI_RX_QUAD)
		tr.rx_nbits = 4;
	else if (mode & SPI_RX_DUAL)
		tr.rx_nbits = 2;
	if (!(mode & SPI_LOOP)) {
		if (mode & (SPI_TX_OCTAL | SPI_TX_QUAD | SPI_TX_DUAL))
			tr.rx_buf = 0;
		else if (mode & (SPI_RX_OCTAL | SPI_RX_QUAD | SPI_RX_DUAL))
			tr.tx_buf = 0;
	}

	ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
	if (ret < 1) {
		perror("can't send spi message");
		return -1;
	}

	if (output_file) {
		out_fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (out_fd < 0) {
			perror("could not open output file");
			return -1;
		}

		write_size = write(out_fd, rx, len);
		if (write_size < 0) {
			perror("not all bytes written to output file");
			close(out_fd);
			return -1;
		}
		if ((size_t)write_size != len) {
			fprintf(stderr,
				"not all bytes written to output file: %zd/%zu\n",
				write_size, len);
			close(out_fd);
			return -1;
		}

		close(out_fd);
	}

	// printf("TX signal: ");
	// for (ret = 0; ret < len; ret++) { //打印接收缓冲区
    //     printf("0x%x ", tx[ret]);
    // }
    // puts("");
    // printf("RX signal: ");
	// for (ret = 0; ret < len; ret++) { //打印接收缓冲区
    //     printf("0x%x ", rx[ret]);
    // }
    // puts("");

	return 0;
}




int spi1_3_init(void)
{
	int ret = 0;

	if (fd >= 0)
		return 0;

	fd = open(spi_device, O_RDWR);
	if (fd < 0) {
		perror("can't open device");
		return -1;
	}

	/*
	 * spi mode
	 */
	ret = ioctl(fd, SPI_IOC_WR_MODE32, &mode);
	if (ret == -1) {
		perror("can't set spi mode");
		goto fail;
	}

	ret = ioctl(fd, SPI_IOC_RD_MODE32, &mode);
	if (ret == -1) {
		perror("can't get spi mode");
		goto fail;
	}

	/*
	 * bits per word
	 */
	ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	if (ret == -1) {
		perror("can't set bits per word");
		goto fail;
	}

	ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
	if (ret == -1) {
		perror("can't get bits per word");
		goto fail;
	}

	/*
	 * max speed hz
	 */
	ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	if (ret == -1) {
		perror("can't set max speed hz");
		goto fail;
	}

	ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
	if (ret == -1) {
		perror("can't get max speed hz");
		goto fail;
	}

	printf("spi mode: 0x%x\n", mode);
	printf("bits per word: %u\n", bits);
	printf("max speed: %u Hz (%u MHz)\n", speed, speed/1000000);

	return 0;

fail:
	close(fd);
	fd = -1;
	return -1;
}

void spi1_3_deinit(void)
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}
