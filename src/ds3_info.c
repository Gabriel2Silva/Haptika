#include "ds3_info.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <libudev.h>
#include <string.h>

/* ── Genuine OUI database ────────────────────────────────────────────────── */

static const char *GENUINE_OUIS[] = {
    "00:02:c7", "00:04:1f", "00:06:f5", "00:06:f7", "00:07:04",
    "00:13:15", "00:15:c1", "00:16:fe", "00:19:c1", "00:19:c5",
    "00:1b:fb", "00:1d:0d", "00:1e:3d", "00:1f:a7", "00:21:4f",
    "00:23:06", "00:24:33", "00:24:8d", "00:26:08", "00:26:43",
    "00:d9:d1", "00:e4:21", "04:76:6e", "04:98:f3", "08:a9:5a",
    "0c:fe:45", "28:0d:fc", "28:a1:83", "2c:cc:44", "30:c3:d9",
    "34:c7:31", "38:c0:96", "44:d8:32", "48:f0:7b", "58:16:d7",
    "60:38:0e", "64:05:e4", "64:d4:bd", "70:9e:29", "74:95:ec",
    "78:c8:81", "9c:8d:7c", "a8:e3:ee", "ac:7a:4d", "b4:ec:02",
    "bc:42:8c", "bc:60:a7", "bc:75:36", "c8:63:f1", "e0:75:0a",
    "e0:ae:5e", "f8:46:1c", "f8:d0:ac", "fc:0f:e6", "fc:62:b9",
};

static gboolean oui_is_genuine(const char *bt_addr) {
    /* bt_addr format: "xx:xx:xx:xx:xx:xx" — extract first 8 chars as OUI */
    if (!bt_addr || strlen(bt_addr) < 8) return FALSE;
    char oui[9];
    g_snprintf(oui, sizeof(oui), "%c%c:%c%c:%c%c",
               g_ascii_tolower(bt_addr[0]), g_ascii_tolower(bt_addr[1]),
               g_ascii_tolower(bt_addr[3]), g_ascii_tolower(bt_addr[4]),
               g_ascii_tolower(bt_addr[6]), g_ascii_tolower(bt_addr[7]));
    for (gsize i = 0; i < G_N_ELEMENTS(GENUINE_OUIS); i++) {
        if (strcmp(oui, GENUINE_OUIS[i]) == 0) return TRUE;
    }
    return FALSE;
}

/* ── Read HID_UNIQ from udev (works over USB and Bluetooth) ─────────────── */

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

/* ── Dialog ──────────────────────────────────────────────────────────────── */

void ds3_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);

    struct udev *udev = udev_new();
    char *bt_addr = (evdev_syspath && udev) ? ds3_get_bt_address(udev, evdev_syspath) : NULL;
    if (udev) udev_unref(udev);

    GtkWidget *toolbar_view = NULL;
    AdwDialog *dialog = controller_info_dialog_new(_("DualShock 3 Info"), 480, 280, &toolbar_view);

    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

    AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, _("Hardware"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(group));

    if (bt_addr) {
        gboolean genuine = oui_is_genuine(bt_addr);

        AdwActionRow *auth_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(auth_row), _("Authenticity"));
        adw_action_row_set_subtitle(auth_row, genuine ? _("Genuine ✓") : _("Clone ⚠"));
        if (!genuine)
            gtk_widget_add_css_class(GTK_WIDGET(auth_row), "error");
        adw_preferences_group_add(group, GTK_WIDGET(auth_row));

        controller_info_add_row(group, _("Bluetooth Address"), bt_addr);

        g_free(bt_addr);
    } else {
        controller_info_add_row(group, _("Bluetooth Address"), _("Not available"));
    }

    adw_dialog_present(dialog, parent);
}

