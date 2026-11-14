#pragma once

#include "device_manager.h"
#include <libudev.h>

G_BEGIN_DECLS

gboolean conttest_vid_pid_is_ds3(uint16_t vid, uint16_t pid);
gboolean conttest_vid_pid_is_ds4(uint16_t vid, uint16_t pid);
gboolean conttest_vid_pid_is_ds5(uint16_t vid, uint16_t pid);

/* Parse VID and PID out of sysfs PRODUCT ("3/54c/ce6/8111") or HID_ID ("0005:0000054C:000009CC"). */
gboolean conttest_parse_vid_pid(const char *str, char sep, uint16_t *v, uint16_t *p);

gboolean conttest_extract_udev_vid_pid(struct udev_device *dev,
                                       uint16_t *vid,
                                       uint16_t *pid);

/* Collect qualifying ancestor hardware keys (hid, bluetooth, input,
 * usb/usb_interface) from an already-opened udev device, plus its own syspath
 * in out_own_syspath. Caller frees returned strv with g_strfreev() and
 * *out_own_syspath with g_free(). */
char **conttest_udev_collect_hw_keys(struct udev_device *dev,
                                     char **out_own_syspath);

/* Check whether two (own_syspath, hw_keys) pairs describe the same physical
 * hardware purely in memory. */
gboolean conttest_hw_keys_match(const char *own_a,
                                const char * const *keys_a,
                                const char *own_b,
                                const char * const *keys_b);

/* Check whether an SDL device node path and an evdev syspath belong to the same
 * physical device. Performs synchronous udev/sysfs lookups; call only from a
 * worker thread or unit tests, never from the GTK main loop. */
gboolean conttest_udev_is_same_hardware(struct udev *udev,
                                        const char *sdl_path,
                                        const char *evdev_syspath);

/* Pure classification of Linux input bustype hex string + parent subsystem flags. */
SDL_JoystickConnectionState conttest_resolve_connection_state_from_props(
    const char *bustype_str,
    gboolean has_usb_parent,
    gboolean has_bt_parent);

/* Resolve connection type from an already-opened udev device. */
SDL_JoystickConnectionState conttest_resolve_connection_state_for_udev_device(
    struct udev_device *dev);

/* Resolve connection type from udev for an evdev syspath or SDL devnode path.
 * Performs synchronous udev/sysfs lookups; call only from a worker thread or
 * unit tests, never from the GTK main loop. */
SDL_JoystickConnectionState conttest_resolve_connection_state_udev(
    struct udev *udev,
    gboolean has_evdev,
    const char *evdev_syspath,
    const char *sdl_path);

G_END_DECLS
