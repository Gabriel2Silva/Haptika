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
        if (uniq) {
            char *clean = controller_info_sanitize_hid_string(uniq, -1);
            if (clean) {
                g_strstrip(clean);
                if (strlen(clean) >= 17)
                    result = clean;
                else
                    g_free(clean);
            }
        }
    }

    udev_device_unref(evdev);
    return result;
}

typedef struct {
    char *bt_addr;
} DS3InfoData;

static void ds3_info_data_free(gpointer p) {
    DS3InfoData *d = p;
    if (!d) return;
    g_free(d->bt_addr);
    g_free(d);
}

static gpointer fetch_ds3_info(const char *evdev_syspath,
                               gboolean is_bt,
                               GCancellable *cancellable) {
    (void)is_bt;
    DS3InfoData *data = g_new0(DS3InfoData, 1);
    if (!evdev_syspath || g_cancellable_is_cancelled(cancellable))
        return data;

    struct udev *udev = udev_new();
    if (udev) {
        data->bt_addr = ds3_get_bt_address(udev, evdev_syspath);
        udev_unref(udev);
    }
    return data;
}

static void on_ds3_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    AdwDialog *dialog = ADW_DIALOG(user_data);
    GError *err = NULL;
    DS3InfoData *data = g_task_propagate_pointer(G_TASK(result), &err);
    GtkWidget *toolbar_view = adw_dialog_get_child(dialog);

    if (!data) {
        controller_info_dialog_show_error(
            toolbar_view,
            _("Could Not Read Device Info"),
            err ? err->message : _("Failed to read device info."));
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);

    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Hardware"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));

    if (data->bt_addr) {
        gboolean genuine = conttest_ds3_oui_is_genuine(data->bt_addr);

        controller_info_add_status_row(group, _("Authenticity"),
                                       genuine ? _("Genuine") : _("Clone"),
                                       genuine ? CONTROLLER_INFO_STATUS_SUCCESS
                                               : CONTROLLER_INFO_STATUS_ERROR);

        controller_info_add_row(group, _("Bluetooth Address"), data->bt_addr);
    } else {
        controller_info_add_row(group, _("Bluetooth Address"), _("Not available"));
    }

    ds3_info_data_free(data);
}

/* Dialog presentation */

void ds3_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);
    if (!evdev_syspath) {
        controller_info_show_error_alert(
            parent,
            _("DualShock 3 Info"),
            _("DualShock 3 info requires kernel evdev and hidraw device access, which is unavailable for this controller."));
        return;
    }

    controller_info_present_async_dialog(parent, _("DualShock 3 Info"), 480, 280,
                                         evdev_syspath, FALSE, FALSE,
                                         fetch_ds3_info, ds3_info_data_free,
                                         on_ds3_fetch_done);
}

