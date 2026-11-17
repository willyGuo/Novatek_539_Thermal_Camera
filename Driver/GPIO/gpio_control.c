#include "gpio_control.h"
#include "../../Config/hardware_config.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_SHUTTER_LINE P_GPIO(0)
#define GPIO_SHUTTER_RETRY_COUNT 3U
#define GPIO_SHUTTER_RETRY_DELAY_US 20000U

static int gpio_shutter_fd = -1;
static GPIO_SHUTTER_FAILURE_CALLBACK gpio_shutter_failure_callback;

void gpio_shutter_set_failure_callback(
	GPIO_SHUTTER_FAILURE_CALLBACK callback)
{
	gpio_shutter_failure_callback = callback;
}

int gpio_open_output(unsigned int line, int initial_value)
{
	struct gpiohandle_request req;
	int chip_fd;

	chip_fd = open(GPIO_CHIP, O_RDONLY);
	if (chip_fd < 0) {
		printf("open fail: %s (%s)\n", GPIO_CHIP, strerror(errno));
		return -1;
	}

	memset(&req, 0, sizeof(req));
	req.lineoffsets[0] = line;
	req.lines = 1;
	req.flags = GPIOHANDLE_REQUEST_OUTPUT;
	req.default_values[0] = initial_value ? 1 : 0;
	snprintf(req.consumer_label, sizeof(req.consumer_label), "gpio_control");

	if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
		printf("get line fail: %s line%u (%s)\n", GPIO_CHIP, line, strerror(errno));
		close(chip_fd);
		return -1;
	}

	close(chip_fd);
	return req.fd;
}

int gpio_set_value(int line_fd, int value)
{
	struct gpiohandle_data data;

	memset(&data, 0, sizeof(data));
	data.values[0] = value ? 1 : 0;

	if (ioctl(line_fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0) {
		printf("set value fail: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

void gpio_close(int line_fd)
{
	if (line_fd >= 0) {
		close(line_fd);
	}
}

int gpio_shutter_init(void)
{
	if (gpio_shutter_fd >= 0) {
		return 0;
	}

	gpio_shutter_fd = gpio_open_output(GPIO_SHUTTER_LINE,
		THERMAL_SHUTTER_OPEN_VALUE);
	if (gpio_shutter_fd < 0) {
		printf("shutter gpio init fail: P_GPIO(0)\r\n");
		return -1;
	}

	if (gpio_set_value(gpio_shutter_fd, THERMAL_SHUTTER_OPEN_VALUE) != 0) {
		printf("shutter gpio initial open (%s) fail\r\n",
		       THERMAL_SHUTTER_OPEN_VALUE ? "high" : "low");
		gpio_close(gpio_shutter_fd);
		gpio_shutter_fd = -1;
		return -1;
	}

	return 0;
}

int gpio_shutter_open(void)
{
	unsigned int attempt;

	for (attempt = 1; attempt <= GPIO_SHUTTER_RETRY_COUNT; attempt++) {
		if (gpio_shutter_init() == 0) {
			printf("open shutter: P_GPIO(0) %s, attempt %u/%u\r\n",
			       THERMAL_SHUTTER_OPEN_VALUE ? "high" : "low",
			       attempt, GPIO_SHUTTER_RETRY_COUNT);
			if (gpio_set_value(gpio_shutter_fd,
				THERMAL_SHUTTER_OPEN_VALUE) == 0) {
				return 0;
			}
		}

		gpio_close(gpio_shutter_fd);
		gpio_shutter_fd = -1;

		if (attempt < GPIO_SHUTTER_RETRY_COUNT) {
			usleep(GPIO_SHUTTER_RETRY_DELAY_US);
		}
	}

	printf("open shutter failed after %u attempts\r\n",
	       GPIO_SHUTTER_RETRY_COUNT);
	if (gpio_shutter_failure_callback != NULL) {
		gpio_shutter_failure_callback();
	}
	return -1;
}

int gpio_shutter_close(void)
{
	unsigned int attempt;

	for (attempt = 1; attempt <= GPIO_SHUTTER_RETRY_COUNT; attempt++) {
		if (gpio_shutter_init() == 0) {
			printf("close shutter: P_GPIO(0) %s, attempt %u/%u\r\n",
			       THERMAL_SHUTTER_CLOSE_VALUE ? "high" : "low",
			       attempt, GPIO_SHUTTER_RETRY_COUNT);
			if (gpio_set_value(gpio_shutter_fd,
				THERMAL_SHUTTER_CLOSE_VALUE) == 0) {
				return 0;
			}
		}

		gpio_close(gpio_shutter_fd);
		gpio_shutter_fd = -1;

		if (attempt < GPIO_SHUTTER_RETRY_COUNT) {
			usleep(GPIO_SHUTTER_RETRY_DELAY_US);
		}
	}

	printf("close shutter failed after %u attempts\r\n",
	       GPIO_SHUTTER_RETRY_COUNT);
	if (gpio_shutter_failure_callback != NULL) {
		gpio_shutter_failure_callback();
	}
	return -1;
}
