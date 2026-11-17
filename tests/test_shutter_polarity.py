"""Compile the production GPIO driver against host-only syscall mocks.
Run: python tests/test_shutter_polarity.py
No board GPIO access, SDK build, or camera operation.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cc = shutil.which("gcc")
assert cc, "gcc required"
env = os.environ.copy()
env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
with tempfile.TemporaryDirectory(prefix="shutter_polarity_") as tmp:
    work = Path(tmp)
    (work/"linux").mkdir()
    (work/"sys").mkdir()
    (work/"fcntl.h").write_text("#define O_RDONLY 0\nint mock_open(const char *, int, ...);\n")
    (work/"unistd.h").write_text("int mock_close(int);\nint mock_usleep(unsigned int);\n")
    (work/"sys/ioctl.h").write_text("int mock_ioctl(int, unsigned long, ...);\n")
    (work/"linux/gpio.h").write_text(r"""
#define GPIOHANDLE_REQUEST_OUTPUT 2U
#define GPIO_GET_LINEHANDLE_IOCTL 1UL
#define GPIOHANDLE_SET_LINE_VALUES_IOCTL 2UL
struct gpiohandle_request {
    unsigned int lineoffsets[64];
    unsigned int flags;
    unsigned char default_values[64];
    char consumer_label[32];
    unsigned int lines;
    int fd;
};
struct gpiohandle_data { unsigned char values[64]; };
""")
    harness = work/"test.c"
    harness.write_text(r"""
#include <assert.h>
#include <stdarg.h>
#define open mock_open
#define close mock_close
#define ioctl mock_ioctl
#define usleep mock_usleep
#include "Driver/GPIO/gpio_control.c"
static int requests, sets, sleeps, callbacks, last_value = -1;
static int fail_sets, fail_all_sets;
static void on_failure(void) { callbacks++; }
int mock_open(const char *path, int flags, ...) {
    assert(strcmp(path, "/dev/gpiochip0") == 0);
    assert(flags == O_RDONLY);
    return 10;
}
int mock_close(int fd) { assert(fd == 10 || fd == 20); return 0; }
int mock_usleep(unsigned int delay) {
    assert(delay == 20000U); sleeps++; return 0;
}
int mock_ioctl(int fd, unsigned long op, ...) {
    va_list ap;
    va_start(ap, op);
    if (op == GPIO_GET_LINEHANDLE_IOCTL) {
        struct gpiohandle_request *req = va_arg(ap, struct gpiohandle_request *);
        assert(fd == 10 && req->lines == 1);
        assert(req->lineoffsets[0] == P_GPIO(0));
        assert(req->flags == GPIOHANDLE_REQUEST_OUTPUT);
        /* Acquisition and retry must immediately request OPEN, including ATTO. */
        assert(req->default_values[0] == EXPECT_OPEN);
        req->fd = 20; requests++;
    } else {
        struct gpiohandle_data *data = va_arg(ap, struct gpiohandle_data *);
        assert(op == GPIOHANDLE_SET_LINE_VALUES_IOCTL && fd == 20);
        sets++;
        if (fail_all_sets || fail_sets > 0) {
            if (fail_sets > 0) fail_sets--;
            va_end(ap); return -1;
        }
        last_value = data->values[0];
    }
    va_end(ap); return 0;
}
int main(void) {
    int count;
    assert(THERMAL_SHUTTER_OPEN_VALUE == EXPECT_OPEN);
    assert(THERMAL_SHUTTER_CLOSE_VALUE == EXPECT_CLOSE);
    gpio_shutter_set_failure_callback(on_failure);
    assert(gpio_shutter_init() == 0 && last_value == EXPECT_OPEN);
    assert(requests == 1 && sets == 1);
    assert(gpio_shutter_init() == 0 && requests == 1 && sets == 1);
    assert(gpio_shutter_close() == 0 && last_value == EXPECT_CLOSE);
    assert(gpio_shutter_open() == 0 && last_value == EXPECT_OPEN);
    /* Repeated FFC-style close/open sequences use the same selected polarity. */
    for (count = 0; count < 3; count++) {
        assert(gpio_shutter_close() == 0 && last_value == EXPECT_CLOSE);
        assert(gpio_shutter_open() == 0 && last_value == EXPECT_OPEN);
    }
    fail_sets = 1;
    assert(gpio_shutter_close() == 0 && last_value == EXPECT_CLOSE);
    assert(requests == 2 && sleeps == 1 && callbacks == 0);
    fail_all_sets = 1;
    count = sleeps;
    assert(gpio_shutter_open() == -1);
    assert(sleeps == count + 2 && callbacks == 1 && gpio_shutter_fd == -1);
    fail_all_sets = 0;
    assert(gpio_shutter_open() == 0 && last_value == EXPECT_OPEN);
    assert(callbacks == 1);
    /* Also exercise initialization failure and its polarity-specific message. */
    gpio_close(gpio_shutter_fd); gpio_shutter_fd = -1;
    fail_sets = 1;
    assert(gpio_shutter_init() == -1 && gpio_shutter_fd == -1);
    assert(gpio_shutter_init() == 0 && last_value == EXPECT_OPEN);
    gpio_close(gpio_shutter_fd);
    printf("PASS: %s open=%d close=%d initialization, FFC sequence, retry and failure\n",
        THERMAL_SENSOR_NAME, EXPECT_OPEN, EXPECT_CLOSE);
    return 0;
}
""")
    for profile, open_value, close_value in ((None,0,1),(1,0,1),(2,1,0)):
        exe = work/(f"profile_{profile}.exe" if os.name == "nt" else f"profile_{profile}")
        command = [cc,"-std=gnu99","-Wall","-Wextra","-Werror",
            "-I",str(work),"-I",str(root),f"-DEXPECT_OPEN={open_value}",
            f"-DEXPECT_CLOSE={close_value}",str(harness),"-o",str(exe)]
        if profile is not None:
            command.append(f"-DTHERMAL_SENSOR={profile}")
        subprocess.run(command,check=True,env=env)
        result = subprocess.run([str(exe)],check=True,env=env,capture_output=True,text=True)
        open_label = "high" if open_value else "low"
        close_label = "high" if close_value else "low"
        assert f"open shutter: P_GPIO(0) {open_label}" in result.stdout
        assert f"close shutter: P_GPIO(0) {close_label}" in result.stdout
        assert f"initial open ({open_label}) fail" in result.stdout
        print(result.stdout.splitlines()[-1])
