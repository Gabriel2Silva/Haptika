/*
 * test_udev_subsys — walk the udev parent chain for input device nodes.
 *
 * Useful for debugging hardware matching (USB vs Bluetooth detection).
 */
#include <libudev.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    struct udev *u = udev_new();
    if (!u) {
        fprintf(stderr, "udev_new failed\n");
        return 1;
    }

    struct udev_enumerate *en = udev_enumerate_new(u);
    udev_enumerate_add_match_subsystem(en, "input");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry;
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(en)) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *dev = udev_device_new_from_syspath(u, path);
        if (!dev) continue;

        const char *devnode = udev_device_get_devnode(dev);
        if (devnode) {
            printf("Node: %s\n", devnode);
            struct udev_device *parent = dev;
            while ((parent = udev_device_get_parent(parent)) != NULL) {
                const char *sub  = udev_device_get_subsystem(parent);
                const char *type = udev_device_get_devtype(parent);
                printf("  -> %s / %s\n",
                       sub ? sub : "(null)", type ? type : "(null)");
                if (sub && strcmp(sub, "usb") == 0 &&
                    type && strcmp(type, "usb_device") == 0)
                    break;
            }
        }
        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
    udev_unref(u);
    return 0;
}
