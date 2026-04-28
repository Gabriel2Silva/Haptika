/*
 * test_udev — list input event nodes with device name properties.
 *
 * Useful for verifying which evdev nodes are visible and how names resolve.
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

        const char *node = udev_device_get_devnode(dev);
        if (node && strncmp(node, "/dev/input/event", 16) == 0) {
            struct udev_device *parent =
                udev_device_get_parent_with_subsystem_devtype(dev, "input", NULL);

            printf("Node: %s\n", node);
            printf("  NAME (prop):       %s\n",
                   udev_device_get_property_value(dev, "NAME") ?: "(null)");
            printf("  name (parent attr): %s\n",
                   parent ? (udev_device_get_sysattr_value(parent, "name") ?: "(null)") : "(no parent)");
        }
        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
    udev_unref(u);
    return 0;
}
