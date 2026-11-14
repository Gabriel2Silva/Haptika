#define _POSIX_C_SOURCE 200809L
#include "device_matching.h"
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

static struct udev_device *udev_device_from_devnode(struct udev *udev,
                                                    const char *path) {
    if (!udev || !path)
        return NULL;
    struct stat st;
    if (stat(path, &st) == 0) {
        return udev_device_new_from_devnum(udev, S_ISBLK(st.st_mode) ? 'b' : 'c',
                                           st.st_rdev);
    }
    return NULL;
}

gboolean conttest_vid_pid_is_ds3(uint16_t vid, uint16_t pid) {
    return vid == SONY_VID && pid == DS3_PID;
}

gboolean conttest_vid_pid_is_ds4(uint16_t vid, uint16_t pid) {
    return vid == SONY_VID &&
           (pid == DS4_PID_V1 ||
            pid == DS4_PID_V2 ||
            pid == DS4_PID_BT);
}

gboolean conttest_vid_pid_is_ds5(uint16_t vid, uint16_t pid) {
    return vid == SONY_VID &&
           (pid == DS5_PID_V1 ||
            pid == DS5_PID_V2);
}

gboolean conttest_parse_vid_pid(const char *str, char sep, uint16_t *v, uint16_t *p) {
    if (!str || !v || !p)
        return FALSE;
    uint16_t tv, tp;
    int matches = (sep == '/')
        ? sscanf(str, "%*x/%hx/%hx", &tv, &tp)
        : sscanf(str, "%*x:%hx:%hx", &tv, &tp);
    if (matches == 2) {
        *v = tv;
        *p = tp;
        return TRUE;
    }
    return FALSE;
}

gboolean conttest_extract_udev_vid_pid(struct udev_device *dev,
                                       uint16_t *vid,
                                       uint16_t *pid) {
    *vid = 0;
    *pid = 0;
    if (!dev)
        return FALSE;

    const char *vid_str = udev_device_get_property_value(dev, "ID_VENDOR_ID");
    const char *pid_str = udev_device_get_property_value(dev, "ID_MODEL_ID");
    if (vid_str)
        *vid = (uint16_t)g_ascii_strtoull(vid_str, NULL, 16);
    if (pid_str)
        *pid = (uint16_t)g_ascii_strtoull(pid_str, NULL, 16);

    if (*vid == 0 || *pid == 0) {
        conttest_parse_vid_pid(udev_device_get_property_value(dev, "PRODUCT"), '/',
                               vid, pid);
    }
    if (*vid == 0 || *pid == 0) {
        struct udev_device *h =
            udev_device_get_parent_with_subsystem_devtype(dev, "hid", NULL);
        if (h)
            conttest_parse_vid_pid(udev_device_get_property_value(h, "HID_ID"), ':',
                                   vid, pid);
    }

    return (*vid != 0 && *pid != 0);
}

char **conttest_udev_collect_hw_keys(struct udev_device *dev,
                                     char **out_own_syspath) {
    if (out_own_syspath)
        *out_own_syspath = NULL;
    if (!dev)
        return NULL;

    if (out_own_syspath)
        *out_own_syspath = g_strdup(udev_device_get_syspath(dev));

    GPtrArray *keys = g_ptr_array_new();
    for (struct udev_device *p = dev; p; p = udev_device_get_parent(p)) {
        const char *sub = udev_device_get_subsystem(p);
        if (!sub)
            continue;
        gboolean match_sub = (g_strcmp0(sub, "hid") == 0 ||
                              g_strcmp0(sub, "bluetooth") == 0 ||
                              g_strcmp0(sub, "input") == 0);
        if (!match_sub && g_strcmp0(sub, "usb") == 0) {
            const char *devtype = udev_device_get_devtype(p);
            match_sub = (g_strcmp0(devtype, "usb_interface") == 0);
        }
        if (match_sub) {
            const char *sp = udev_device_get_syspath(p);
            if (sp)
                g_ptr_array_add(keys, g_strdup(sp));
        }
    }
    g_ptr_array_add(keys, NULL);
    return (char **)g_ptr_array_free(keys, FALSE);
}

gboolean conttest_hw_keys_match(const char *own_a,
                                const char * const *keys_a,
                                const char *own_b,
                                const char * const *keys_b) {
    if (own_a && own_b && g_strcmp0(own_a, own_b) == 0)
        return TRUE;
    if (!keys_a || !keys_b)
        return FALSE;
    for (size_t i = 0; keys_a[i]; i++) {
        for (size_t j = 0; keys_b[j]; j++) {
            if (g_strcmp0(keys_a[i], keys_b[j]) == 0)
                return TRUE;
        }
    }
    return FALSE;
}

gboolean conttest_udev_is_same_hardware(struct udev *udev,
                                        const char *sdl_path,
                                        const char *evdev_syspath) {
    if (!udev || !sdl_path || !evdev_syspath)
        return FALSE;

    struct udev_device *u1 = udev_device_from_devnode(udev, sdl_path);
    struct udev_device *u2 = udev_device_new_from_syspath(udev, evdev_syspath);
    gboolean match = FALSE;

    if (u1 && u2) {
        g_autofree char *own1 = NULL;
        g_autofree char *own2 = NULL;
        g_auto(GStrv) keys1 = conttest_udev_collect_hw_keys(u1, &own1);
        g_auto(GStrv) keys2 = conttest_udev_collect_hw_keys(u2, &own2);
        match = conttest_hw_keys_match(own1, (const char * const *)keys1,
                                       own2, (const char * const *)keys2);
    }
    if (u1)
        udev_device_unref(u1);
    if (u2)
        udev_device_unref(u2);
    return match;
}

SDL_JoystickConnectionState conttest_resolve_connection_state_from_props(
    const char *bustype_str,
    gboolean has_usb_parent,
    gboolean has_bt_parent) {
    if (bustype_str && *bustype_str) {
        char *end = NULL;
        unsigned long bustype = strtoul(bustype_str, &end, 16);
        if (end != bustype_str && (*end == '\0' || g_ascii_isspace(*end))) {
            if (bustype == BUS_USB)
                return SDL_JOYSTICK_CONNECTION_WIRED;
            if (bustype == BUS_BLUETOOTH)
                return SDL_JOYSTICK_CONNECTION_WIRELESS;
            /* BUS_VIRTUAL (uinput), BUS_I2C, BUS_RS232 (serio), BUS_HOST, etc. stay UNKNOWN. */
        }
        return SDL_JOYSTICK_CONNECTION_UNKNOWN;
    }

    if (has_usb_parent)
        return SDL_JOYSTICK_CONNECTION_WIRED;
    if (has_bt_parent)
        return SDL_JOYSTICK_CONNECTION_WIRELESS;
    return SDL_JOYSTICK_CONNECTION_UNKNOWN;
}

SDL_JoystickConnectionState conttest_resolve_connection_state_for_udev_device(
    struct udev_device *dev) {
    if (!dev)
        return SDL_JOYSTICK_CONNECTION_UNKNOWN;

    struct udev_device *input_parent =
        udev_device_get_parent_with_subsystem_devtype(dev, "input", NULL);
    const char *bustype_str =
        input_parent
            ? udev_device_get_sysattr_value(input_parent, "id/bustype")
            : NULL;

    gboolean has_usb_parent = FALSE;
    gboolean has_bt_parent = FALSE;
    if (!bustype_str || !*bustype_str) {
        has_usb_parent = udev_device_get_parent_with_subsystem_devtype(dev, "usb", NULL) != NULL;
        if (!has_usb_parent)
            has_bt_parent = udev_device_get_parent_with_subsystem_devtype(dev, "bluetooth", NULL) != NULL;
    }

    return conttest_resolve_connection_state_from_props(bustype_str, has_usb_parent, has_bt_parent);
}

SDL_JoystickConnectionState conttest_resolve_connection_state_udev(
    struct udev *udev,
    gboolean has_evdev,
    const char *evdev_syspath,
    const char *sdl_path) {
    if (!udev)
        return SDL_JOYSTICK_CONNECTION_UNKNOWN;

    struct udev_device *dev = NULL;
    if (has_evdev && evdev_syspath)
        dev = udev_device_new_from_syspath(udev, evdev_syspath);
    else if (sdl_path && *sdl_path)
        dev = udev_device_from_devnode(udev, sdl_path);
    if (!dev)
        return SDL_JOYSTICK_CONNECTION_UNKNOWN;

    SDL_JoystickConnectionState conn =
        conttest_resolve_connection_state_for_udev_device(dev);
    udev_device_unref(dev);
    return conn;
}
