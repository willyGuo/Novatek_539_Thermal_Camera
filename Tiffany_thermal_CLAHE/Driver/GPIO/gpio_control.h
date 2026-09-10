#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

#define P_GPIO(pin) ((pin) + 0x20)

typedef void (*GPIO_SHUTTER_FAILURE_CALLBACK)(void);

int gpio_open_output(unsigned int line, int initial_value);
int gpio_set_value(int line_fd, int value);
void gpio_close(int line_fd);

void gpio_shutter_set_failure_callback(
	GPIO_SHUTTER_FAILURE_CALLBACK callback);
int gpio_shutter_init(void);
int gpio_shutter_open(void);
int gpio_shutter_close(void);

#endif
