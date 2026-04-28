#include "ds5_info.h"
#include "ds4_info.h" /* find_hidraw_node */
#include "controller_info_utils.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static guint8 *ds5_get_feature(int fd, guint8 id, gsize size) {
    guint8 *buf = g_malloc0(size);
    buf[0] = id;
    if (ioctl(fd, HIDIOCGFEATURE(size), buf) < 0) { g_free(buf); return NULL; }
    return buf;
}

/* Send 0x80 [base, num], receive 0x81, return bytes [4..4+len] as hex or string.
 * Returns newly-allocated string or NULL on error. */
static char *ds5_get_system_info(int fd, guint8 base, guint8 num, gsize length, gboolean as_hex) {
    guint8 req[3] = { 0x80, base, num };
    if (ioctl(fd, HIDIOCSFEATURE(3), req) < 0) return NULL;

    guint8 *resp = ds5_get_feature(fd, 0x81, 4 + length + 4);
    if (!resp) return NULL;
    if (resp[0] != 0x81 || resp[1] != base || resp[2] != num || resp[3] != 2) { g_free(resp); return g_strdup("error"); }

    char *result;
    if (as_hex) {
        GString *s = g_string_new("0x");
        for (gsize i = 0; i < length; i++)
            g_string_append_printf(s, "%02x", resp[4 + i]);
        result = g_string_free(s, FALSE);
    } else {
        result = g_strndup((char *)(resp + 4), length);
        /* Trim trailing nulls/spaces */
        g_strchomp(result);
    }
    g_free(resp);
    return result;
}

/* ── Board model ─────────────────────────────────────────────────────────── */

static const char *ds5_hw_to_board_model(guint32 hwinfo) {
    guint8 a = (hwinfo >> 8) & 0xff;
    if (a == 0x03) return "BDM-010";
    if (a == 0x04) return "BDM-020";
    if (a == 0x05) return "BDM-030";
    if (a == 0x06) return "BDM-040";
    if (a == 0x07 || a == 0x08) return "BDM-050";
    if (a == 0x11) return "BDM-060M";
    if (a == 0x13) return "BDM-060X";
    return "Unknown";
}

/* ── Color from serial number ────────────────────────────────────────────── */

static const char *ds5_color_from_serial(const char *serial) {
    if (!serial || strlen(serial) < 6) return "Unknown";
    char code[3] = { serial[4], serial[5], '\0' };
    if (!strcmp(code, "00")) return "White";
    if (!strcmp(code, "01")) return "Midnight Black";
    if (!strcmp(code, "02")) return "Cosmic Red";
    if (!strcmp(code, "03")) return "Nova Pink";
    if (!strcmp(code, "04")) return "Galactic Purple";
    if (!strcmp(code, "05")) return "Starlight Blue";
    if (!strcmp(code, "06")) return "Grey Camouflage";
    if (!strcmp(code, "07")) return "Volcanic Red";
    if (!strcmp(code, "08")) return "Sterling Silver";
    if (!strcmp(code, "09")) return "Cobalt Blue";
    if (!strcmp(code, "10")) return "Chroma Teal";
    if (!strcmp(code, "11")) return "Chroma Indigo";
    if (!strcmp(code, "12")) return "Chroma Pearl";
    if (!strcmp(code, "30")) return "30th Anniversary";
    if (!strcmp(code, "Z1")) return "God of War Ragnarök";
    if (!strcmp(code, "Z2")) return "Spider-Man 2";
    if (!strcmp(code, "Z3")) return "Astro Bot";
    if (!strcmp(code, "Z4")) return "Fortnite";
    if (!strcmp(code, "Z6")) return "The Last of Us";
    if (!strcmp(code, "ZB")) return "Icon Blue Limited Edition";
    return "Unknown";
}

/* ── Data struct ─────────────────────────────────────────────────────────── */

typedef struct {
    /* Firmware */
    char fw_build_date[32];
    char fw_type[12];
    char fw_series[12];
    char fw_version[12];
    char fw_update[12];
    char fw_update_info[8];
    char sbl_fw_version[12];
    char venom_fw_version[12];
    char spider_fw_version[12];
    /* Hardware */
    char color[64];
    char serial_number[32];
    char mcu_unique_id[32];
    char pcba_id[32];
    char battery_barcode[32];
    char vcm_left_barcode[32];
    char vcm_right_barcode[32];
    char board_model[16];
    char hw_model[12];
    char bt_address[18];
    gboolean is_bt;
    gboolean ok;
    char error_msg[128];
} DS5InfoData;

/* Reverse a hex string in-place (byte-swap for PCBA ID) */
static char *reverse_hex_bytes(const char *hex) {
    if (!hex) return g_strdup("error");
    gsize len = strlen(hex);
    if (len < 2 || len % 2 != 0) return g_strdup(hex);
    char *out = g_malloc(len + 1);
    for (gsize i = 0; i < len / 2; i++) {
        out[i * 2]     = hex[len - 2 - i * 2];
        out[i * 2 + 1] = hex[len - 1 - i * 2];
    }
    out[len] = '\0';
    return out;
}

static DS5InfoData *fetch_ds5_info(const char *hidraw_node, gboolean is_bt) {
    DS5InfoData *info = g_new0(DS5InfoData, 1);
    info->is_bt = is_bt;

    int fd = open(hidraw_node, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        int saved_errno = errno;
        snprintf(info->error_msg, sizeof(info->error_msg),
                 "Cannot open %s: %s", hidraw_node, g_strerror(saved_errno));
        return info;
    }

    /* Feature report 0x20: main firmware/hardware info (64 bytes) */
    guint8 *r20 = ds5_get_feature(fd, 0x20, 64);
    if (!r20 || r20[0] != 0x20) {
        g_free(r20);
        snprintf(info->error_msg, sizeof(info->error_msg), "Feature report 0x20 failed");
        close(fd);
        return info;
    }

    /* FW Build Date: bytes 1–11 (date) + 12–19 (time) */
    char date[12] = {0}, time_s[9] = {0};
    memcpy(date,   r20 + 1,  11);
    memcpy(time_s, r20 + 12, 8);
    snprintf(info->fw_build_date, sizeof(info->fw_build_date), "%s %s", date, time_s);
    g_strchomp(info->fw_build_date);

    guint16 fwtype    = (guint16)(r20[20] | (r20[21] << 8));
    guint16 swseries  = (guint16)(r20[22] | (r20[23] << 8));
    guint32 hwinfo    = (guint32)(r20[24] | (r20[25] << 8) | (r20[26] << 16) | (r20[27] << 24));
    guint32 fwversion = (guint32)(r20[28] | (r20[29] << 8) | (r20[30] << 16) | (r20[31] << 24));
    guint16 updver    = (guint16)(r20[44] | (r20[45] << 8));
    guint8  unk       = r20[46];
    guint32 fwver1    = (guint32)(r20[48] | (r20[49] << 8) | (r20[50] << 16) | (r20[51] << 24));
    guint32 fwver2    = (guint32)(r20[52] | (r20[53] << 8) | (r20[54] << 16) | (r20[55] << 24));
    guint32 fwver3    = (guint32)(r20[56] | (r20[57] << 8) | (r20[58] << 16) | (r20[59] << 24));
    g_free(r20);

    snprintf(info->fw_type,         sizeof(info->fw_type),         "0x%04x", fwtype);
    snprintf(info->fw_series,       sizeof(info->fw_series),       "0x%04x", swseries);
    snprintf(info->fw_version,      sizeof(info->fw_version),      "0x%08x", fwversion);
    snprintf(info->fw_update,       sizeof(info->fw_update),       "0x%04x", updver);
    snprintf(info->fw_update_info,  sizeof(info->fw_update_info),  "0x%02x", unk);
    snprintf(info->sbl_fw_version,  sizeof(info->sbl_fw_version),  "0x%08x", fwver1);
    snprintf(info->venom_fw_version,sizeof(info->venom_fw_version),"0x%08x", fwver2);
    snprintf(info->spider_fw_version,sizeof(info->spider_fw_version),"0x%08x", fwver3);
    snprintf(info->board_model,     sizeof(info->board_model),     "%s", ds5_hw_to_board_model(hwinfo));
    snprintf(info->hw_model,        sizeof(info->hw_model),        "0x%08x", hwinfo);

    /* getSystemInfo calls — unreliable over Bluetooth */
    if (!is_bt) {
        char *serial = ds5_get_system_info(fd, 1, 19, 17, FALSE);
        snprintf(info->serial_number, sizeof(info->serial_number), "%s", serial ? serial : "error");
        snprintf(info->color, sizeof(info->color), "%s", ds5_color_from_serial(serial));
        g_free(serial);

        char *mcu = ds5_get_system_info(fd, 1, 9, 9, TRUE);
        snprintf(info->mcu_unique_id, sizeof(info->mcu_unique_id), "%s", mcu ? mcu : "error");
        g_free(mcu);

        char *pcba_hex = ds5_get_system_info(fd, 1, 17, 14, TRUE);
        if (pcba_hex) {
            char *pcba_rev = reverse_hex_bytes(pcba_hex + 2); /* skip "0x" prefix */
            snprintf(info->pcba_id, sizeof(info->pcba_id), "%s", pcba_rev);
            g_free(pcba_rev);
            g_free(pcba_hex);
        } else {
            snprintf(info->pcba_id, sizeof(info->pcba_id), "error");
        }

        char *bat = ds5_get_system_info(fd, 1, 24, 23, FALSE);
        snprintf(info->battery_barcode, sizeof(info->battery_barcode), "%s", bat ? bat : "error");
        g_free(bat);

        char *vcml = ds5_get_system_info(fd, 1, 26, 16, FALSE);
        snprintf(info->vcm_left_barcode, sizeof(info->vcm_left_barcode), "%s", vcml ? vcml : "error");
        g_free(vcml);

        char *vcmr = ds5_get_system_info(fd, 1, 28, 16, FALSE);
        snprintf(info->vcm_right_barcode, sizeof(info->vcm_right_barcode), "%s", vcmr ? vcmr : "error");
        g_free(vcmr);

        /* Bluetooth address: send 0x80 [9,2], receive 0x81, bytes [4..9] reversed */
        guint8 req[3] = { 0x80, 9, 2 };
        ioctl(fd, HIDIOCSFEATURE(3), req);
        guint8 *resp = ds5_get_feature(fd, 0x81, 16);
        if (resp) {
            snprintf(info->bt_address, sizeof(info->bt_address),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     resp[9], resp[8], resp[7], resp[6], resp[5], resp[4]);
            g_free(resp);
        } else {
            snprintf(info->bt_address, sizeof(info->bt_address), "N/A");
        }
    }

    close(fd);
    info->ok = TRUE;
    return info;
}

/* ── GTask async wrapper ─────────────────────────────────────────────────── */

typedef struct { char *hidraw_node; gboolean is_bt; } DS5FetchTaskData;

static void ds5_fetch_task_data_free(gpointer p) {
    DS5FetchTaskData *d = p;
    g_free(d->hidraw_node);
    g_free(d);
}

static void ds5_fetch_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
    (void)source; (void)cancellable;
    DS5FetchTaskData *d = (DS5FetchTaskData *)task_data;
    g_task_return_pointer(task, fetch_ds5_info(d->hidraw_node, d->is_bt), g_free);
}


static void on_ds5_fetch_done(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)source;
    AdwDialog *dialog = ADW_DIALOG(user_data);
    DS5InfoData *info = g_task_propagate_pointer(G_TASK(result), NULL);

    GtkWidget *toolbar_view = adw_dialog_get_child(dialog);
    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);


    if (!info || !info->ok) {
        GtkWidget *lbl = gtk_label_new(info ? info->error_msg : _("Failed to read device info."));
        gtk_widget_add_css_class(lbl, "dim-label");
        gtk_box_append(GTK_BOX(box), lbl);
    } else {
        if (info->is_bt) {
            AdwBanner *banner = ADW_BANNER(adw_banner_new(
                _("Some fields are only available over USB")));
            adw_banner_set_revealed(banner, TRUE);
            adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(banner));
        }

        AdwPreferencesGroup *fw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(fw, _("Software"));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(fw));
        controller_info_add_row(fw, _("FW Build Date"),       info->fw_build_date);
        controller_info_add_row(fw, _("FW Type"),             info->fw_type);
        controller_info_add_row(fw, _("FW Series"),           info->fw_series);
        controller_info_add_row(fw, _("FW Version"),          info->fw_version);
        controller_info_add_row(fw, _("FW Update"),           info->fw_update);
        controller_info_add_row(fw, _("FW Update Info"),      info->fw_update_info);
        controller_info_add_row(fw, _("SBL FW Version"),      info->sbl_fw_version);
        controller_info_add_row(fw, _("Venom FW Version"),    info->venom_fw_version);
        controller_info_add_row(fw, _("Spider FW Version"),   info->spider_fw_version);

        AdwPreferencesGroup *hw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(hw, _("Hardware"));
        gtk_box_append(GTK_BOX(box), GTK_WIDGET(hw));
        controller_info_add_row(hw, _("Board Model"),         info->board_model);
        controller_info_add_row(hw, _("HW Model"),            info->hw_model);
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

    g_free(info);
    g_object_unref(dialog);
}

void ds5_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const char *evdev_syspath = conttest_device_get_evdev_syspath(device);
    if (!evdev_syspath) {
        AdwDialog *d = ADW_DIALOG(adw_alert_dialog_new(_("DualSense Info"), NULL));
        adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
            _("DualSense info requires evdev access. Please select the evdev backend."));
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
        adw_dialog_present(d, parent);
        return;
    }

    struct udev *udev = udev_new();
    char *hidraw_node = (udev) ? find_hidraw_node(udev, evdev_syspath) : NULL;
    if (udev) udev_unref(udev);
    if (!hidraw_node) {
        AdwDialog *d = ADW_DIALOG(adw_alert_dialog_new(_("DualSense Info"), NULL));
        adw_alert_dialog_set_body(ADW_ALERT_DIALOG(d),
            _("Could not find hidraw device. You may need to add a udev rule to allow access."));
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(d), "ok", _("OK"));
        adw_dialog_present(d, parent);
        return;
    }

    GtkWidget *toolbar_view = NULL;
    AdwDialog *dialog = controller_info_dialog_new(_("DualSense Info"), 480, 500, &toolbar_view);

    adw_dialog_present(dialog, parent);

    GTask *task = g_task_new(NULL, NULL, on_ds5_fetch_done, g_object_ref(dialog));
    DS5FetchTaskData *td = g_new(DS5FetchTaskData, 1);
    td->hidraw_node = hidraw_node;
    td->is_bt = (conttest_device_get_connection_state(device) == SDL_JOYSTICK_CONNECTION_WIRELESS);
    g_task_set_task_data(task, td, ds5_fetch_task_data_free);
    g_task_run_in_thread(task, ds5_fetch_thread);
    g_object_unref(task);
}
