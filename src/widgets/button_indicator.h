#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_TYPE_BUTTON_INDICATOR (conttest_button_indicator_get_type())
G_DECLARE_FINAL_TYPE(ConttestButtonIndicator, conttest_button_indicator, CONTTEST, BUTTON_INDICATOR, GtkWidget)

GtkWidget *conttest_button_indicator_new(const char *label);
void conttest_button_indicator_set_pressed(ConttestButtonIndicator *self, gboolean pressed);

G_END_DECLS
