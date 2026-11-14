#include "ds5_info.h"
#include "controller_info_parsers.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gi18n.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Send 0x80 [base, num], receive 0x81, return bytes [4..4+len] as hex or sanitized UTF-8.
 * Returns a newly-allocated string (caller must g_free) or NULL on error. */
static char *ds5_get_system_info(int fd, guint8 base, guint8 num, gsize length, gboolean as_hex) {
    if (length == 0 || length > 256)
        return NULL;

    guint8 req[3] = { 0x80, base, num };
    if (ioctl(fd, HIDIOCSFEATURE(3), req) < 0)
        return NULL;

    gsize resp_len = 0;
    guint8 *resp = controller_info_hidraw_get_feature(fd, 0x81, 4 + length + 4, 4 + length, &resp_len);
    if (!resp)
        return NULL;

    char *result = conttest_ds5_parse_system_info_response(resp, resp_len,
                                                            base, num, length, as_hex);
    g_free(resp);
    return result;
}

/* DualSense info data */

typedef struct {
    ConttestDS5Report20 r20;
    /* Hardware */
    char color[64];
    char serial_number[64];
    char mcu_unique_id[32];
    char pcba_id[32];
    char battery_barcode[80];
    char vcm_left_barcode[64];
    char vcm_right_barcode[64];
    char bt_address[32];
    gboolean is_bt;
    gboolean ok;
    char error_msg[256];
} DS5InfoData;

static void ds5_copy_system_info(int fd, guint8 base, guint8 num,
                                 gsize length, gboolean as_hex,
                                 char *dst, gsize dst_size,
                                 GCancellable *cancellable) {
    if (g_cancellable_is_cancelled(cancellable))
        return;
    char *val = ds5_get_system_info(fd, base, num, length, as_hex);
    controller_info_copy_utf8(dst, dst_size, val ? val : _("Not available"));
    g_free(val);
}

static gpointer fetch_ds5_info(const char *hidraw_node,
                               gboolean is_bt,
                               GCancellable *cancellable) {
    DS5InfoData *info = g_new0(DS5InfoData, 1);
    info->is_bt = is_bt;

    int fd = open(hidraw_node, O_RDWR | O_NONBLOCK);
    if (fd < 0 && (errno == EACCES || errno == EPERM))
        fd = open(hidraw_node, O_RDONLY | O_NONBLOCK);
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

    /* Feature report 0x20: main firmware/hardware info (64 bytes, need >= 60). */
    gsize r20_len = 0;
    guint8 *r20 = controller_info_hidraw_get_feature(fd, 0x20, 64, 60, &r20_len);
    if (!conttest_ds5_parse_report_20(r20, r20_len, &info->r20)) {
        g_free(r20);
        controller_info_copy_utf8(info->error_msg, sizeof(info->error_msg),
                                  _("Feature report 0x20 failed"));
        close(fd);
        return info;
    }
    g_free(r20);

    /* getSystemInfo queries are unreliable over Bluetooth; run over USB only. */
    if (!is_bt && !g_cancellable_is_cancelled(cancellable)) {
        char *serial = ds5_get_system_info(fd, 1, 19, 17, FALSE);
        controller_info_copy_utf8(info->serial_number, sizeof(info->serial_number),
                                  serial ? serial : _("Not available"));
        controller_info_copy_utf8(info->color, sizeof(info->color),
                                  conttest_ds5_color_from_serial(serial));
        g_free(serial);

        ds5_copy_system_info(fd, 1, 9, 9, TRUE,
                             info->mcu_unique_id, sizeof(info->mcu_unique_id),
                             cancellable);

        if (!g_cancellable_is_cancelled(cancellable)) {
            char *pcba_hex = ds5_get_system_info(fd, 1, 17, 14, TRUE);
            char *pcba_rev = (pcba_hex && strlen(pcba_hex) > 2)
                ? conttest_ds5_reverse_hex_bytes(pcba_hex + 2)
                : NULL;
            controller_info_copy_utf8(info->pcba_id, sizeof(info->pcba_id),
                                      pcba_rev ? pcba_rev : _("Not available"));
            g_free(pcba_rev);
            g_free(pcba_hex);
        }

        ds5_copy_system_info(fd, 1, 24, 23, FALSE,
                             info->battery_barcode, sizeof(info->battery_barcode),
                             cancellable);
        ds5_copy_system_info(fd, 1, 26, 16, FALSE,
                             info->vcm_left_barcode, sizeof(info->vcm_left_barcode),
                             cancellable);
        ds5_copy_system_info(fd, 1, 28, 16, FALSE,
                             info->vcm_right_barcode, sizeof(info->vcm_right_barcode),
                             cancellable);

        /* Bluetooth address: send 0x80 [9, 2], receive 0x81, bytes [4..9] reversed. */
        if (!g_cancellable_is_cancelled(cancellable)) {
            guint8 req[3] = { 0x80, 9, 2 };
            gsize resp_len = 0;
            guint8 *resp = NULL;
            if (ioctl(fd, HIDIOCSFEATURE(3), req) >= 0)
                resp = controller_info_hidraw_get_feature(fd, 0x81, 16, 10, &resp_len);
            if (!conttest_ds5_parse_bt_addr_response(resp, resp_len, info->bt_address, sizeof(info->bt_address))) {
                controller_info_copy_utf8(info->bt_address, sizeof(info->bt_address),
                                          _("Not available"));
            }
            g_free(resp);
        }
    }

    close(fd);
    info->ok = TRUE;
    return info;
}

static void on_ds5_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    AdwDialog *dialog = ADW_DIALOG(user_data);
    GError *err = NULL;
    DS5InfoData *info = g_task_propagate_pointer(G_TASK(result), &err);
    GtkWidget *toolbar_view = adw_dialog_get_child(dialog);

    if (!info || !info->ok) {
        const char *desc = (info && info->error_msg[0] != '\0')
            ? info->error_msg
            : (err ? err->message : _("Failed to read device info."));
        controller_info_dialog_show_error(
            toolbar_view,
            _("Could Not Read Device Info"),
            desc);
    } else {
        GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

        if (info->is_bt) {
            AdwBanner *banner = ADW_BANNER(adw_banner_new(
                _("Some fields are only available over USB")));
            adw_banner_set_revealed(banner, TRUE);
            adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(banner));
        }

        AdwPreferencesGroup *fw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(fw, _("Software"));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(fw));
        controller_info_add_row(fw, _("FW Build Date"),       info->r20.fw_build_date);
        controller_info_add_row(fw, _("FW Type"),             info->r20.fw_type);
        controller_info_add_row(fw, _("FW Series"),           info->r20.fw_series);
        controller_info_add_row(fw, _("FW Version"),          info->r20.fw_version);
        controller_info_add_row(fw, _("FW Update"),           info->r20.fw_update);
        controller_info_add_row(fw, _("FW Update Info"),      info->r20.fw_update_info);
        controller_info_add_row(fw, _("SBL FW Version"),      info->r20.sbl_fw_version);
        controller_info_add_row(fw, _("Venom FW Version"),    info->r20.venom_fw_version);
        controller_info_add_row(fw, _("Spider FW Version"),   info->r20.spider_fw_version);

        AdwPreferencesGroup *hw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(hw, _("Hardware"));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(hw));
        controller_info_add_row(hw, _("Board Model"),         info->r20.board_model);
        controller_info_add_row(hw, _("HW Model"),            info->r20.hw_model);
        if (!info->is_bt) {
            controller_info_add_row(hw, _("Color"),               info->color);
            controller_info_add_row(hw, _("Serial Number"),       info->serial_number);
            controller_info_add_row(hw, _("MCU Unique ID"),       info->mcu_unique_id);
            controller_info_add_row(hw, _("PCBA ID"),             info->pcba_id);
            controller_info_add_row(hw, _("Battery Barcode"),     info->battery_barcode);
            controller_info_add_row(hw, _("VCM Left Barcode"),    info->vcm_left_barcode);
            controller_info_add_row(hw, _("VCM Right Barcode"),   info->vcm_right_barcode);
            controller_info_add_row(hw, _("Bluetooth Address"),   info->bt_address);
        }
    }

    g_clear_error(&err);
    g_free(info);
}

void ds5_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);
    if (!evdev_syspath) {
        controller_info_show_error_alert(
            parent,
            _("DualSense Info"),
            _("DualSense info requires kernel evdev and hidraw device access, which is unavailable for this controller."));
        return;
    }

    gboolean is_bt = (conttest_device_get_connection_state(device) == SDL_JOYSTICK_CONNECTION_WIRELESS);
    controller_info_present_async_dialog(parent, _("DualSense Info"), 480, 500,
                                         evdev_syspath, TRUE, is_bt,
                                         fetch_ds5_info, g_free,
                                         on_ds5_fetch_done);
}
