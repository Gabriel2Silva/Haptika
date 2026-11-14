#include "ds4_info.h"
#include "device_manager.h"
#include "controller_info_parsers.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <unistd.h>

/* DualShock 4 hardware info payload */

typedef struct {
    ConttestDS4ReportA3 a3;
    char bt_address[32];
    gboolean has_bt_addr;
    gboolean is_clone;
    gboolean is_bt;
    gboolean ok;
    char error_msg[256];
} DS4InfoData;

static gpointer fetch_ds4_info(const char *hidraw_node,
                               gboolean is_bt,
                               GCancellable *cancellable) {
    DS4InfoData *info = g_new0(DS4InfoData, 1);
    info->is_bt = is_bt;

    int fd = open(hidraw_node, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        int saved_errno = errno;
        char *msg = g_strdup_printf(_("Cannot open %s: %s"), hidraw_node, g_strerror(saved_errno));
        controller_info_copy_utf8(info->error_msg, sizeof(info->error_msg), msg);
        g_free(msg);
        return info;
    }

    if (g_cancellable_is_cancelled(cancellable)) {
        close(fd);
        return info;
    }

    /* Feature report 0xa3: build date, HW/SW version (49 bytes, reads through 0x2a). */
    gsize r_len = 0;
    guint8 *r = controller_info_hidraw_get_feature(fd, 0xa3, 49, 0x2b, &r_len);
    if (!conttest_ds4_parse_report_a3(r, r_len, &info->a3)) {
        g_free(r);
        info->is_clone = TRUE;
        controller_info_copy_utf8(info->error_msg, sizeof(info->error_msg),
                                  _("Feature report 0xa3 failed (clone?)"));
    } else {
        g_free(r);

        /* Feature report 0x81 confirms genuine Sony hardware over USB; skip over BT. */
        if (is_bt) {
            info->is_clone = FALSE;
        } else if (!g_cancellable_is_cancelled(cancellable)) {
            guint8 *r81 = controller_info_hidraw_get_feature(fd, 0x81, 7, 7, NULL);
            info->is_clone = (r81 == NULL || r81[0] != 0x81);
            g_free(r81);
        }

        info->ok = TRUE;
    }

    /* Feature report 0x12: Bluetooth address (6 bytes at offset 1, reversed). */
    if (!g_cancellable_is_cancelled(cancellable)) {
        gsize r12_len = 0;
        guint8 *r12 = controller_info_hidraw_get_feature(fd, 0x12, 16, 7, &r12_len);
        if (conttest_ds4_parse_report_12_bt(r12, r12_len, info->bt_address, sizeof(info->bt_address)))
            info->has_bt_addr = TRUE;
        g_free(r12);
    }

    close(fd);
    return info;
}

static void on_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    AdwDialog *dialog = ADW_DIALOG(user_data);
    GError *err = NULL;
    DS4InfoData *info = g_task_propagate_pointer(G_TASK(result), &err);
    GtkWidget *toolbar_view = adw_dialog_get_child(dialog);

    if (!info || (!info->ok && !info->has_bt_addr)) {
        const char *desc = (info && info->error_msg[0] != '\0')
            ? info->error_msg
            : (err ? err->message : _("Failed to read device info."));
        controller_info_dialog_show_error(
            toolbar_view,
            _("Could Not Read Device Info"),
            desc);
    } else {
        GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

        AdwPreferencesGroup *hw_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(hw_group, _("Hardware"));
        if (!info->ok && info->error_msg[0] != '\0')
            adw_preferences_group_set_description(hw_group, info->error_msg);
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(hw_group));

        const char *auth;
        ControllerInfoStatus auth_status;
        if (info->is_bt) {
            auth = _("Unknown — connect via USB to test authenticity");
            auth_status = CONTROLLER_INFO_STATUS_NONE;
        } else if (info->is_clone) {
            auth = _("Clone");
            auth_status = CONTROLLER_INFO_STATUS_ERROR;
        } else {
            auth = _("Genuine");
            auth_status = CONTROLLER_INFO_STATUS_SUCCESS;
        }
        controller_info_add_status_row(hw_group, _("Authenticity"), auth, auth_status);

        if (info->ok) {
            controller_info_add_row(hw_group, _("Board Model"), info->a3.board_model);
            controller_info_add_row(hw_group, _("HW Version"),  info->a3.hw_version);
        }
        controller_info_add_row(hw_group, _("Bluetooth Address"),
                                info->has_bt_addr ? info->bt_address : _("Not available"));

        if (info->ok) {
            AdwPreferencesGroup *fw_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(fw_group, _("Firmware"));
            gtk_box_append(GTK_BOX(box), GTK_WIDGET(fw_group));

            controller_info_add_row(fw_group, _("Build Date"), info->a3.build_date);
            controller_info_add_row(fw_group, _("SW Version"), info->a3.sw_version);
        }
    }

    g_clear_error(&err);
    g_free(info);
}

void ds4_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);
    if (!evdev_syspath) {
        controller_info_show_error_alert(
            parent,
            _("DualShock 4 Info"),
            _("DualShock 4 info requires kernel evdev and hidraw device access, which is unavailable for this controller."));
        return;
    }

    gboolean is_bt = (conttest_device_get_connection_state(device) == SDL_JOYSTICK_CONNECTION_WIRELESS);
    controller_info_present_async_dialog(parent, _("DualShock 4 Info"), 400, 640,
                                         evdev_syspath, TRUE, is_bt,
                                         fetch_ds4_info, g_free,
                                         on_fetch_done);
}
