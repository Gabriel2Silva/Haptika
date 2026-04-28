#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_TYPE_TRIGGER_BAR (conttest_trigger_bar_get_type())
G_DECLARE_FINAL_TYPE(ConttestTriggerBar, conttest_trigger_bar, CONTTEST, TRIGGER_BAR, GtkBox)

GtkWidget *conttest_trigger_bar_new(const char *name);
void conttest_trigger_bar_set_value(ConttestTriggerBar *self, double value);

G_END_DECLS
