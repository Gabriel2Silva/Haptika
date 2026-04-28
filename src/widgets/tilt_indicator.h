#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_TYPE_TILT_INDICATOR (conttest_tilt_indicator_get_type())
G_DECLARE_FINAL_TYPE(ConttestTiltIndicator, conttest_tilt_indicator, CONTTEST, TILT_INDICATOR, GtkWidget)

GtkWidget *conttest_tilt_indicator_new(void);
/* ax, ay, az in m/s² (raw accelerometer values including gravity) */
void conttest_tilt_indicator_set_accel(ConttestTiltIndicator *self, float ax, float ay, float az);

G_END_DECLS
