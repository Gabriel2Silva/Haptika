#include "ds4_info.h"
#include "device_manager.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── hidraw helpers ──────────────────────────────────────────────────────── */

/* Find the hidraw devnode for the device at evdev_syspath.
 * udev must be a valid, caller-owned udev context.
 * Returns a newly-allocated string or NULL. Caller must g_free(). */
char *find_hidraw_node(struct udev *udev, const char *evdev_syspath) {
    char *result = NULL;

    struct udev_device *evdev = udev_device_new_from_syspath(udev, evdev_syspath);
    if (!evdev) return NULL;

    struct udev_device *hid = udev_device_get_parent_with_subsystem_devtype(evdev, "hid", NULL);
    if (!hid) goto out_evdev;

    struct udev_enumerate *en = udev_enumerate_new(udev);
    udev_enumerate_add_match_subsystem(en, "hidraw");
    udev_enumerate_add_match_parent(en, hid);
    udev_enumerate_scan_devices(en);

    struct udev_list_entry *entry = udev_enumerate_get_list_entry(en);
    if (entry) {
        const char *path = udev_list_entry_get_name(entry);
        struct udev_device *hr = udev_device_new_from_syspath(udev, path);
        if (hr) {
            const char *node = udev_device_get_devnode(hr);
            if (node) result = g_strdup(node);
            udev_device_unref(hr);
        }
    }
    udev_enumerate_unref(en);

out_evdev:
    udev_device_unref(evdev);
    return result;
}

/* Send a GET_FEATURE ioctl and return a newly-allocated buffer, or NULL.
 * buf[0] must be set to the report ID before calling. */
static guint8 *hidraw_get_feature(int fd, guint8 report_id, gsize buf_size) {
    guint8 *buf = g_malloc0(buf_size);
    buf[0] = report_id;
    int ret = ioctl(fd, HIDIOCGFEATURE(buf_size), buf);
    if (ret < 0) {
        g_free(buf);
        return NULL;
    }
    return buf;
}

/* ── DS4 info data ───────────────────────────────────────────────────────── */

typedef struct {
    char build_date[64];
    char hw_version[16];
    char sw_version[16];
    char board_model[32];
    char bt_address[18];
    gboolean is_clone;
    gboolean is_bt;
    gboolean ok;
    char error_msg[128];
} DS4InfoData;

static const char *hw_to_board_model(guint16 hw_ver_minor) {
    guint8 a = hw_ver_minor >> 8;
    if (a == 0x31) return "JDM-001";
    if (a == 0x43) return "JDM-011";
    if (a == 0x54) return "JDM-030";
    if (a >= 0x64 && a <= 0x74) return "JDM-040";
    if ((a > 0x80 && a < 0x84) || a == 0x93) return "JDM-020";
    if (a == 0xa4 || a == 0x90 || a == 0xa0) return "JDM-050";
    if (a == 0xb0) return "JDM-055 (Scuf?)";
    if (a == 0xb4) return "JDM-055";
    return "Unknown";
}

static DS4InfoData *fetch_ds4_info(const char *hidraw_node, gboolean is_bt) {
    DS4InfoData *info = g_new0(DS4InfoData, 1);
    info->is_bt = is_bt;

    int fd = open(hidraw_node, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        int saved_errno = errno;
        snprintf(info->error_msg, sizeof(info->error_msg),
                 "Cannot open %s: %s", hidraw_node, g_strerror(saved_errno));
        return info;
    }

    /* Feature report 0xa3: build date, HW/SW version */
    guint8 *r = hidraw_get_feature(fd, 0xa3, 49);
    if (!r || r[0] != 0xa3) {
        g_free(r);
        info->is_clone = TRUE;
        snprintf(info->error_msg, sizeof(info->error_msg), "Feature report 0xa3 failed (clone?)");
        /* Still try to get BT address */
    } else {
        /* Build date: bytes 1–0x0f and 0x10–0x1f, null-terminated strings */
        char k1[16] = {0}, k2[16] = {0};
        memcpy(k1, r + 1,    15); k1[15] = '\0';
        memcpy(k2, r + 0x10, 16); k2[15] = '\0';
        snprintf(info->build_date, sizeof(info->build_date), "%s %s", k1, k2);
        /* Trim trailing spaces/nulls */
        g_strchomp(info->build_date);

        guint16 hw_major = (guint16)(r[0x21] | (r[0x22] << 8));
        guint16 hw_minor = (guint16)(r[0x23] | (r[0x24] << 8));
        guint32 sw_major = (guint32)(r[0x25] | (r[0x26] << 8) | (r[0x27] << 16) | (r[0x28] << 24));
        guint16 sw_minor = (guint16)(r[0x29] | (r[0x2a] << 8));

        snprintf(info->hw_version, sizeof(info->hw_version), "%04x:%04x", hw_major, hw_minor);
        snprintf(info->sw_version, sizeof(info->sw_version), "%08x:%04x", sw_major, sw_minor);
        snprintf(info->board_model, sizeof(info->board_model), "%s", hw_to_board_model(hw_minor));

        g_free(r);

        /* Feature report 0x81: present on genuine controllers only.
         * Clone detection method matches dualshock-tools.github.io:
         *   1. If 0xa3 fails entirely → clone (handled above).
         *   2. If 0x81 GET_FEATURE fails → clone.
         *   3. If 0x81 succeeds → genuine.
         * Unreliable over Bluetooth — skip the check in that case. */
        if (is_bt) {
            info->is_clone = FALSE; /* indeterminate; handled in UI */
        } else {
            guint8 *r81 = hidraw_get_feature(fd, 0x81, 7);
            info->is_clone = (r81 == NULL || r81[0] != 0x81);
            g_free(r81);
        }

        info->ok = TRUE;
    }

    /* Feature report 0x12: Bluetooth address (6 bytes at offset 1, reversed) */
    guint8 *r12 = hidraw_get_feature(fd, 0x12, 16);
    if (r12) {
        snprintf(info->bt_address, sizeof(info->bt_address),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 r12[6], r12[5], r12[4], r12[3], r12[2], r12[1]);
        g_free(r12);
    } else {
        snprintf(info->bt_address, sizeof(info->bt_address), "N/A");
    }

    close(fd);
    return info;
}

/* ── GTask async wrapper ─────────────────────────────────────────────────── */

typedef struct { char *hidraw_node; gboolean is_bt; } FetchTaskData;

static void fetch_task_data_free(gpointer p) {
    FetchTaskData *d = p;
    g_free(d->hidraw_node);
    g_free(d);
}

static void fetch_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
    (void)source; (void)cancellable;
    FetchTaskData *d = (FetchTaskData *)task_data;
    DS4InfoData *info = fetch_ds4_info(d->hidraw_node, d->is_bt);
    g_task_return_pointer(task, info, g_free);
}


static void on_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    AdwDialog *dialog = ADW_DIALOG(user_data);

    DS4InfoData *info = g_task_propagate_pointer(G_TASK(result), NULL);

    GtkWidget *toolbar_view = adw_dialog_get_child(dialog);
    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);


    if (!info || (!info->ok && info->bt_address[0] == '\0')) {
        GtkWidget *lbl = gtk_label_new(info ? info->error_msg : _("Failed to read device info."));
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_box_append(GTK_BOX(box), lbl);
    } else {
        AdwPreferencesGroup *hw_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(hw_group, _("Hardware"));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(hw_group));

        if (info->ok) {
            const char *auth;
            if (info->is_bt)
                auth = _("Unknown — connect via USB to test authenticity");
            else
                auth = info->is_clone ? _("Clone ⚠") : _("Genuine ✓");
            controller_info_add_row(hw_group, _("Authenticity"), auth);
            controller_info_add_row(hw_group, _("Board Model"), info->board_model);
            controller_info_add_row(hw_group, _("HW Version"),  info->hw_version);
        }
        controller_info_add_row(hw_group, _("Bluetooth Address"), info->bt_address);

        if (info->ok) {
            AdwPreferencesGroup *fw_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(fw_group, _("Firmware"));
            gtk_box_append(GTK_BOX(box), GTK_WIDGET(fw_group));

            controller_info_add_row(fw_group, _("Build Date"),  info->build_date);
            controller_info_add_row(fw_group, _("SW Version"),  info->sw_version);
        }
    }

    g_free(info);
    g_object_unref(dialog);
}

void ds4_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);
    if (!evdev_syspath) {
        AdwDialog *d = ADW_DIALOG(adw_alert_dialog_new(_("DS4 Info"), NULL));
        adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
            _("DS4 info requires evdev access. Please select the evdev backend."));
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
        adw_dialog_present(d, parent);
        return;
    }

    struct udev *udev = udev_new();
    char *hidraw_node = (udev) ? find_hidraw_node(udev, evdev_syspath) : NULL;
    if (udev) udev_unref(udev);
    if (!hidraw_node) {
        AdwDialog *d = ADW_DIALOG(adw_alert_dialog_new(_("DS4 Info"), NULL));
        adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
            _("Could not find hidraw device. You may need to add a udev rule to allow access."));
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
        adw_dialog_present(d, parent);
        return;
    }

    /* Build the dialog with a spinner while loading */
    GtkWidget *toolbar_view = NULL;
    AdwDialog *dialog = controller_info_dialog_new(_("DualShock 4 Info"), 400, 640, &toolbar_view);


    adw_dialog_present(dialog, parent);

    /* Fetch asynchronously */
    FetchTaskData *td = g_new(FetchTaskData, 1);
    td->hidraw_node = hidraw_node;
    td->is_bt = (conttest_device_get_connection_state(device) == SDL_JOYSTICK_CONNECTION_WIRELESS);
    GTask *task = g_task_new(NULL, NULL, on_fetch_done, g_object_ref(dialog));
    g_task_set_task_data(task, td, fetch_task_data_free);
    g_task_run_in_thread(task, fetch_thread);
    g_object_unref(task);
}
