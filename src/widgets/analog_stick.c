#include "analog_stick.h"
#include "drawing_utils.h"
#include <math.h>

#define STICK_MIN_SIZE 100
#define STICK_NAT_SIZE 100

struct _ConttestAnalogStick {
    GtkWidget  parent_instance;
    GtkWidget *label;
    int        canvas_w;
    int        canvas_h;
    double     stick_x;
    double     stick_y;
};

G_DEFINE_TYPE(ConttestAnalogStick, conttest_analog_stick, GTK_TYPE_WIDGET)

static void conttest_analog_stick_dispose(GObject *object) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(object);
    g_clear_pointer(&self->label, gtk_widget_unparent);
    G_OBJECT_CLASS(conttest_analog_stick_parent_class)->dispose(object);
}

static void conttest_analog_stick_measure(GtkWidget *widget,
                                          GtkOrientation orientation,
                                          int for_size,
                                          int *minimum,
                                          int *natural,
                                          int *minimum_baseline,
                                          int *natural_baseline) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(widget);
    conttest_captioned_widget_measure(self->label, orientation, for_size,
                                      STICK_MIN_SIZE, STICK_MIN_SIZE,
                                      STICK_NAT_SIZE, STICK_NAT_SIZE,
                                      minimum, natural,
                                      minimum_baseline, natural_baseline);
}

static void conttest_analog_stick_size_allocate(GtkWidget *widget,
                                                int width,
                                                int height,
                                                int baseline) {
    (void)baseline;
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(widget);
    conttest_captioned_widget_allocate(self->label, width, height,
                                       &self->canvas_w, &self->canvas_h);
}

static void conttest_analog_stick_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    ConttestAnalogStick *self = CONTTEST_ANALOG_STICK(widget);
    conttest_snapshot_circular_indicator(widget, snapshot,
                                         (float)self->canvas_w,
                                         (float)self->canvas_h,
                                         (float)self->stick_x,
                                         (float)self->stick_y);
    if (self->label && gtk_widget_get_visible(self->label))
        gtk_widget_snapshot_child(widget, self->label, snapshot);
}

static void conttest_analog_stick_init(ConttestAnalogStick *self) {
    self->label = conttest_captioned_widget_create_label(GTK_WIDGET(self), NULL);
    conttest_widget_connect_style_redraw(GTK_WIDGET(self));
}

static void conttest_analog_stick_class_init(ConttestAnalogStickClass *klass) {
    GObjectClass *oclass = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);

    oclass->dispose = conttest_analog_stick_dispose;
    wclass->measure = conttest_analog_stick_measure;
    wclass->size_allocate = conttest_analog_stick_size_allocate;
    wclass->snapshot = conttest_analog_stick_snapshot;

    gtk_widget_class_set_accessible_role(wclass, GTK_ACCESSIBLE_ROLE_IMG);
}

GtkWidget *conttest_analog_stick_new(const char *name) {
    ConttestAnalogStick *self = g_object_new(CONTTEST_TYPE_ANALOG_STICK, NULL);
    conttest_captioned_widget_set_caption(GTK_WIDGET(self), self->label, name);
    return GTK_WIDGET(self);
}

void conttest_analog_stick_set_position(ConttestAnalogStick *self, double x, double y) {
    g_return_if_fail(CONTTEST_IS_ANALOG_STICK(self));
    if (isnan(x) || isnan(y))
        return;
    if (fabs(x - self->stick_x) < 0.005 && fabs(y - self->stick_y) < 0.005)
        return;
    self->stick_x = x;
    self->stick_y = y;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
