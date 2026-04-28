/*
 * test_match — test udev device-from-devnum lookup.
 *
 * Usage: test_match [devnode]
 *   Defaults to /dev/null if no argument is given.
 *   Pass a real device node (e.g. /dev/input/event0) to test hardware lookup.
 */
#include <libudev.h>
#include <stdio.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    const char *path = (argc > 1) ? argv[1] : "/dev/null";

    struct udev *u = udev_new();
    if (!u) {
        fprintf(stderr, "udev_new failed\n");
        return 1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        perror(path);
        udev_unref(u);
        return 1;
    }

    struct udev_device *dev = udev_device_new_from_devnum(
        u, S_ISBLK(st.st_mode) ? 'b' : 'c', st.st_rdev);
    if (dev) {
        printf("Device for %s:\n", path);
        printf("  syspath:   %s\n", udev_device_get_syspath(dev));
        printf("  subsystem: %s\n", udev_device_get_subsystem(dev) ?: "(null)");
        printf("  devtype:   %s\n", udev_device_get_devtype(dev) ?: "(null)");
        udev_device_unref(dev);
    } else {
        printf("No udev device for %s\n", path);
    }

    udev_unref(u);
    return 0;
}
