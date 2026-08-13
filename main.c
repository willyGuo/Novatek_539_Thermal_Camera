#include "app.h"

int main(void)
{
	int ret;

	ret = app_init();
	if (ret != 0) {
		return ret;
	}

	ret = app_run();
	app_deinit();
	return ret;
}
