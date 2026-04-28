#pragma once
#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

/* Creates an AdwDialog with a toolbar view, header bar, and content spinner.
 * Returns the dialog; out_toolbar_view (if non-NULL) receives the toolbar. */
AdwDialog *controller_info_dialog_new(const char *title, int width, int height,
                                       GtkWidget **out_toolbar_view);

/* Replaces the spinner in toolbar_view with a clamp and a vertical box. 
 * Returns the box to add preferences groups to. */
GtkWidget *controller_info_dialog_show_content(GtkWidget *toolbar_view);

/* Adds a selectable AdwActionRow to group with title and value. */
void controller_info_add_row(AdwPreferencesGroup *group,
                              const char *title, const char *value);

G_END_DECLS
