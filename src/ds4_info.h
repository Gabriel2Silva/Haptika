#pragma once
#include <adwaita.h>
#include <libudev.h>
#include "device_manager.h"

G_BEGIN_DECLS

/* Show the DS4 info dialog, parented to `parent`. Fetches data asynchronously. */
void ds4_info_show(GtkWidget *parent, const ConttestDevice *device);

/* Shared utility: find the hidraw devnode for the device at evdev_syspath.
 * udev must be a valid, caller-owned udev context.
 * Returns a newly-allocated string or NULL. Caller must g_free(). */
char *find_hidraw_node(struct udev *udev, const char *evdev_syspath);

G_END_DECLS
