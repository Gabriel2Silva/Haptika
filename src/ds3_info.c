#include "ds3_info.h"
#include "controller_info_parsers.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <libudev.h>
#include <string.h>

/* Read HID_UNIQ from udev (available over both USB and Bluetooth). */

static char *ds3_get_bt_address(struct udev *udev, const char *evdev_syspath) {
    char *result = NULL;
    struct udev_device *evdev = udev_device_new_from_syspath(udev, evdev_syspath);
    if (!evdev) return NULL;

    struct udev_device *hid = udev_device_get_parent_with_subsystem_devtype(evdev, "hid", NULL);
    if (hid) {
        const char *uniq = udev_device_get_property_value(hid, "HID_UNIQ");
        if (uniq && strlen(uniq) >= 17)
            result = g_strdup(uniq);
    }

    udev_device_unref(evdev);
    return result;
}

/* Dialog presentation */

void ds3_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);

    struct udev *udev = udev_new();
    char *bt_addr = (evdev_syspath && udev) ? ds3_get_bt_address(udev, evdev_syspath) : NULL;
    if (udev) udev_unref(udev);

    GtkWidget *toolbar_view = NULL;
    AdwDialog *dialog = controller_info_dialog_new(_("DualShock 3 Info"), 480, 280,
                                                   FALSE, &toolbar_view);

    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Hardware"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));

    if (bt_addr) {
        gboolean genuine = conttest_ds3_oui_is_genuine(bt_addr);

        controller_info_add_status_row(group, _("Authenticity"),
                                       genuine ? _("Genuine") : _("Clone"),
                                       genuine ? CONTROLLER_INFO_STATUS_SUCCESS
                                               : CONTROLLER_INFO_STATUS_ERROR);

        controller_info_add_row(group, _("Bluetooth Address"), bt_addr);

        g_free(bt_addr);
    } else {
        controller_info_add_row(group, _("Bluetooth Address"), _("Not available"));
    }

    adw_dialog_present(dialog, parent);
}

