#include "controller_info_utils.h"

AdwDialog *controller_info_dialog_new(const char *title, int width, int height,
                                       GtkWidget **out_toolbar_view) {
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, title);
    if (width > 0) adw_dialog_set_content_width(dialog, width);
    if (height > 0) adw_dialog_set_content_height(dialog, height);

    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_dialog_set_child(dialog, toolbar_view);

    GtkWidget *header = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

    GtkWidget *spinner = adw_spinner_new();
    gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), spinner);

    if (out_toolbar_view)
        *out_toolbar_view = toolbar_view;

    return dialog;
}

GtkWidget *controller_info_dialog_show_content(GtkWidget *toolbar_view) {
    GtkWidget *old_content = adw_toolbar_view_get_content(ADW_TOOLBAR_VIEW(toolbar_view));
    if (old_content) {
        gtk_widget_set_visible(old_content, FALSE);
    }
    
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), scroll);

    GtkWidget *clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 480);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_top(box, 24);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);

    return box;
}

void controller_info_add_row(AdwPreferencesGroup *group,
                              const char *title, const char *value) {
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    adw_preferences_group_add(group, GTK_WIDGET(row));
}
