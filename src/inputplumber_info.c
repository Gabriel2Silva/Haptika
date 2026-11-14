#include "inputplumber_info.h"
#include "controller_info_utils.h"
#include <adwaita.h>
#include <glib/gi18n.h>

static char *format_intercept_mode(guint32 mode) {
    switch (mode) {
    case 0:  return g_strdup(_("None"));
    case 1:  return g_strdup(_("Pass"));
    case 2:  return g_strdup(_("All"));
    case 3:  return g_strdup(_("Gamepad Only"));
    default: return g_strdup_printf("%u", mode);
    }
}

void inputplumber_info_show(GtkWidget *parent, const ConttestDevice *device) {
    const ConttestInputPlumberInfo *info =
        device ? conttest_device_get_inputplumber_info(device) : NULL;
    if (!info) return;

    GtkWidget *toolbar_view = NULL;
    AdwDialog *dialog = controller_info_dialog_new(_("InputPlumber Info"), 480, 560,
                                                   FALSE, &toolbar_view);
    GtkWidget *box = controller_info_dialog_show_content(toolbar_view);

    /* InputPlumber Composite Device group */
    AdwPreferencesGroup *ip_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(ip_group, _("InputPlumber"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(ip_group));

    controller_info_add_row(ip_group, _("Composite Device"), info->composite_name);
    controller_info_add_row(ip_group, _("Target Type"),      info->target_type);
    controller_info_add_row(ip_group, _("Profile"),          info->profile_name);
    controller_info_add_row(ip_group, _("Profile Path"),     info->profile_path);
    controller_info_add_row(ip_group, _("Persistent ID"),    info->persistent_id);

    char *intercept_str = format_intercept_mode(info->intercept_mode);
    controller_info_add_row(ip_group, _("Intercept Mode"), intercept_str);
    g_free(intercept_str);

    controller_info_add_row(ip_group, _("Daemon Version"), info->daemon_version);

    /* Source Hardware groups */
    if (info->sources && info->sources->len > 0) {
        for (guint i = 0; i < info->sources->len; i++) {
            const ConttestInputPlumberSource *src = g_ptr_array_index(info->sources, i);
            AdwPreferencesGroup *src_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            if (info->sources->len == 1) {
                adw_preferences_group_set_title(src_group, _("Source Hardware"));
            } else {
                char *title = g_strdup_printf(_("Source Hardware %u"), i + 1);
                adw_preferences_group_set_title(src_group, title);
                g_free(title);
            }
            gtk_box_append(GTK_BOX(box), GTK_WIDGET(src_group));

            controller_info_add_row(src_group, _("Device Name"),   src->name);
            if (src->vendor_id && *src->vendor_id && src->product_id && *src->product_id) {
                char *hwid = g_strdup_printf("%s:%s", src->vendor_id, src->product_id);
                controller_info_add_row(src_group, _("Hardware ID"), hwid);
                g_free(hwid);
            }
            controller_info_add_row(src_group, _("Device Node"),   src->device_path);
            controller_info_add_row(src_group, _("Physical Path"), src->phys_path);
            controller_info_add_row(src_group, _("Subsystem"),     src->subsystem);
        }
    }

    /* Emulated Target group */
    AdwPreferencesGroup *tgt_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(tgt_group, _("Emulated Target"));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(tgt_group));

    controller_info_add_row(tgt_group, _("Target Name"),      info->target_name);
    controller_info_add_row(tgt_group, _("Virtual Bus"),      info->virtual_bus);
    controller_info_add_row(tgt_group, _("Emulated Address"), info->target_uniq);

    if (info->output_caps && info->output_caps->len > 0) {
        GString *caps = g_string_new(NULL);
        for (guint i = 0; i < info->output_caps->len; i++) {
            if (i > 0) g_string_append(caps, ", ");
            g_string_append(caps, g_ptr_array_index(info->output_caps, i));
        }
        controller_info_add_row(tgt_group, _("Output Capabilities"), caps->str);
        g_string_free(caps, TRUE);
    } else {
        controller_info_add_row(tgt_group, _("Output Capabilities"), _("None"));
    }

    adw_dialog_present(dialog, parent);
}
