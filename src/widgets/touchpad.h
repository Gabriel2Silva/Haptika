#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CONTTEST_TOUCHPAD_MAX_FINGERS 8

#define CONTTEST_TYPE_TOUCHPAD (conttest_touchpad_get_type())
G_DECLARE_FINAL_TYPE(ConttestTouchpad, conttest_touchpad, CONTTEST, TOUCHPAD, GtkWidget)

GtkWidget *conttest_touchpad_new(void);
void       conttest_touchpad_set_finger(ConttestTouchpad *self, gboolean active, float x, float y);
void       conttest_touchpad_set_finger_indexed(ConttestTouchpad *self, guint finger_idx,
                                                gboolean active, float x, float y);

G_END_DECLS
