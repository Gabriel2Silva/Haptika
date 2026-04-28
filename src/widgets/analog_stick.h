#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_TYPE_ANALOG_STICK (conttest_analog_stick_get_type())
G_DECLARE_FINAL_TYPE(ConttestAnalogStick, conttest_analog_stick, CONTTEST, ANALOG_STICK, GtkWidget)

GtkWidget *conttest_analog_stick_new(const char *name);
void conttest_analog_stick_set_position(ConttestAnalogStick *self, double x, double y);

G_END_DECLS
